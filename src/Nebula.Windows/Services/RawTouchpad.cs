using Microsoft.UI.Dispatching;
using System.Buffers.Binary;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace Nebula_Windows.Services;

/// Two-finger touchpad orbit that is free in both axes.
///
/// Windows turns precision-touchpad scrolling into wheel events that are locked to one axis
/// ("rails") until the motion clearly leaves it. Instead of intercepting that, this listens
/// beside it: Raw Input delivers the touchpad's own HID contact reports (usage page 0x0D,
/// usage 0x05) without consuming anything, so we read both fingers' positions and orbit by the
/// movement of their centroid. While fingers are down, or our momentum is running, the railed
/// wheel events for the same gesture are ignored by the caller (<see cref="OwnsWheel"/>).
/// Pinch stays with the system's Ctrl+wheel; a mostly-pinching frame does not orbit.
internal sealed class RawTouchpad : IDisposable
{
    private const uint WmInput = 0x00FF;
    private const uint RidInput = 0x10000003;
    private const uint RidiPreparsedData = 0x20000005;
    private const uint RimTypeHid = 2;
    private const uint RidevInputSink = 0x00000100;
    private const int HeaderSize = 24;          // RAWINPUTHEADER on 64-bit
    private const int HidpStatusSuccess = 0x00110000;
    private const double PointsPerMillimetre = 5.0; // scroll points per mm of finger travel, unaccelerated
    private const double MomentumTau = 0.325;       // seconds; momentum decays ~0.95 per 16 ms frame

    private readonly nint _window;
    private readonly Action<float, float> _orbitPoints;
    private readonly Action<string> _log;
    private readonly Func<bool> _canStart;
    private readonly DispatcherQueue _queue;
    private readonly SubclassProcedure _procedure;
    private readonly nuint _subclassId = (nuint)Random.Shared.NextInt64(1, long.MaxValue);
    private readonly Dictionary<nint, Device?> _devices = [];
    private readonly Dictionary<uint, Contact> _contacts = [];   // fingers down as of the last complete frame
    private readonly Dictionary<uint, Contact> _frame = [];      // fingers down in the frame being read
    private int _framesLogged;
    private byte[] _buffer = new byte[512];
    private bool _subclassed, _disposed;

    // Current frame (hybrid touchpads send one finger per report; parallel ones send all at once).
    private int _expected, _received;
    // Gesture
    private (uint A, uint B)? _pair;
    private double _lastX, _lastY, _lastSpread;
    private long _lastReportTicks, _lastFrameTicks;
    private double _vx, _vy;                    // points per second, smoothed
    private double _gestureX, _gestureY, _gestureMix;
    private int _gestureFrames;
    private DispatcherQueueTimer? _momentum;
    private long _momentumTicks;

    public RawTouchpad(nint window, Func<bool> canStart, Action<float, float> orbitPoints, Action<string> log)
    {
        _window = window;
        _canStart = canStart;
        _orbitPoints = orbitPoints;
        _log = log;
        _queue = DispatcherQueue.GetForCurrentThread() ?? throw new InvalidOperationException("RawTouchpad needs the UI thread.");
        _procedure = WindowProcedure;
        var device = new RawInputDevice { UsagePage = 0x0D, Usage = 0x05, Flags = RidevInputSink, Target = window };
        if (!RegisterRawInputDevices([device], 1, (uint)Marshal.SizeOf<RawInputDevice>()))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not register for precision-touchpad input.");
        if (!SetWindowSubclass(window, _procedure, _subclassId, 0))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not listen for touchpad input.");
        _subclassed = true;
        _log("Touchpad: listening for precision-touchpad contacts (Raw Input, nothing intercepted).");
    }

    /// True while fingers are on the pad, just after (the system's own wheel events trail the
    /// contacts slightly), or while our momentum runs: wheel events then belong to this gesture.
    public bool OwnsWheel =>
        _pair is not null || _momentum?.IsRunning == true ||
        Environment.TickCount64 - _lastReportTicks < 250 && _contacts.Count > 0;

    /// Any input that means "stop": a click, a new gesture, a key.
    public void StopMomentum() => _momentum?.Stop();

    private nint WindowProcedure(nint hwnd, uint message, nuint wParam, nint lParam, nuint id, nuint data)
    {
        if (message == WmInput && !_disposed)
        {
            try { OnInput(lParam); }
            catch (Exception error) { _log($"Touchpad: input ignored: {error.Message}"); }
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    private unsafe void OnInput(nint handle)
    {
        uint size = 0;
        if (GetRawInputData(handle, RidInput, null, ref size, HeaderSize) != 0 || size == 0) return;
        if (_buffer.Length < size) _buffer = new byte[Math.Max(size, _buffer.Length * 2)];
        fixed (byte* data = _buffer)
            if (GetRawInputData(handle, RidInput, data, ref size, HeaderSize) != size) return;
        var span = _buffer.AsSpan(0, (int)size);
        if (BinaryPrimitives.ReadUInt32LittleEndian(span) != RimTypeHid) return;
        var deviceHandle = (nint)BinaryPrimitives.ReadInt64LittleEndian(span[8..]);
        var device = DeviceFor(deviceHandle);
        if (device is null) return;
        var reportSize = (int)BinaryPrimitives.ReadUInt32LittleEndian(span[HeaderSize..]);
        var count = (int)BinaryPrimitives.ReadUInt32LittleEndian(span[(HeaderSize + 4)..]);
        for (var r = 0; r < count; r++)
        {
            var offset = HeaderSize + 8 + r * reportSize;
            if (offset + reportSize > span.Length) break;
            ReadReport(device, _buffer, offset, reportSize);
        }
    }

    private unsafe void ReadReport(Device device, byte[] buffer, int offset, int length)
    {
        _lastReportTicks = Environment.TickCount64;
        fixed (byte* report = &buffer[offset])
        fixed (byte* preparsed = device.Preparsed)
        {
            // A report that carries a contact count starts a frame. Parallel-mode pads send every
            // finger slot in that one report; hybrid pads send one slot per report with the count
            // only in the first. Either way only the first <count> slots are real: the rest are
            // empty (contact 0, not touching) and must not be read as a finger lifting.
            uint contactCount = 0;
            var hasCount = device.ContactCountCollection is ushort cc &&
                HidP_GetUsageValue(0, 0x0D, cc, 0x54, out contactCount, preparsed, report, (uint)length) == HidpStatusSuccess &&
                contactCount > 0;
            if (hasCount)
            {
                if (_expected > 0) EndFrame();
                _expected = (int)contactCount;
                _received = 0;
                _frame.Clear();
            }
            else if (_expected == 0) return; // a stray continuation with no frame to belong to
            var usages = stackalloc ushort[32];
            foreach (var finger in device.Fingers)
            {
                if (HidP_GetUsageValue(0, 0x01, finger.Collection, 0x30, out var x, preparsed, report, (uint)length) != HidpStatusSuccess ||
                    HidP_GetUsageValue(0, 0x01, finger.Collection, 0x31, out var y, preparsed, report, (uint)length) != HidpStatusSuccess)
                    continue;
                uint contactId = 0;
                if (finger.HasContactId) HidP_GetUsageValue(0, 0x0D, finger.Collection, 0x51, out contactId, preparsed, report, (uint)length);
                uint n = 32;
                var tip = false;
                if (HidP_GetUsages(0, 0x0D, finger.Collection, usages, ref n, preparsed, report, (uint)length) == HidpStatusSuccess)
                    for (var k = 0; k < n; k++) if (usages[k] == 0x42) { tip = true; break; }
                if (_received >= _expected) break;
                _received++;
                if (tip)
                    _frame[contactId] = new Contact((x - device.MinX) * device.MillimetresPerUnitX,
                        (y - device.MinY) * device.MillimetresPerUnitY);
            }
        }
        if (_received >= _expected) EndFrame();
    }

    private void EndFrame()
    {
        _expected = 0;
        _received = 0;
        _contacts.Clear();
        foreach (var (id, contact) in _frame) _contacts[id] = contact;
        _frame.Clear();
        if (_framesLogged < 8 && _contacts.Count > 0)
        {
            _framesLogged++;
            _log($"Touchpad: frame {string.Join(" ", _contacts.Select(c => $"#{c.Key}@{c.Value.X:F1},{c.Value.Y:F1}mm"))}");
        }
        var now = Environment.TickCount64;
        if (_contacts.Count == 0) _framesLogged = 0;
        if (_contacts.Count != 2)
        {
            if (_pair is not null) EndGesture();
            return;
        }
        var ids = _contacts.Keys.Order().ToArray();
        var a = _contacts[ids[0]];
        var b = _contacts[ids[1]];
        var cx = (a.X + b.X) / 2;
        var cy = (a.Y + b.Y) / 2;
        var spread = Math.Sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y));
        if (_pair != (ids[0], ids[1]))
        {
            if (_pair is not null) EndGesture();
            if (!_canStart()) return;
            _momentum?.Stop();
            _pair = (ids[0], ids[1]);
            (_lastX, _lastY, _lastSpread, _lastFrameTicks) = (cx, cy, spread, now);
            _vx = _vy = 0;
            _gestureX = _gestureY = _gestureMix = 0;
            _gestureFrames = 0;
            return;
        }
        var dx = (cx - _lastX) * PointsPerMillimetre;
        var dy = (cy - _lastY) * PointsPerMillimetre;
        var dSpread = Math.Abs(spread - _lastSpread) * PointsPerMillimetre;
        var dt = Math.Clamp((now - _lastFrameTicks) / 1000.0, 0.004, 0.1);
        (_lastX, _lastY, _lastSpread, _lastFrameTicks) = (cx, cy, spread, now);
        // Pinching moves the centroid a little; let the zoom have that frame.
        if (dSpread > 1.5 * Math.Sqrt(dx * dx + dy * dy)) { _vx = _vy = 0; return; }
        if (dx == 0 && dy == 0) return;
        _orbitPoints((float)dx, (float)dy);
        const double smoothing = 0.35;
        _vx += (dx / dt - _vx) * smoothing;
        _vy += (dy / dt - _vy) * smoothing;
        _gestureX += Math.Abs(dx);
        _gestureY += Math.Abs(dy);
        _gestureMix += Math.Min(Math.Abs(dx), Math.Abs(dy));
        _gestureFrames++;
    }

    private void EndGesture()
    {
        _framesLogged = 0;
        if (_gestureFrames > 0)
        {
            var axisMix = _gestureMix / Math.Max(1e-6, Math.Max(_gestureX, _gestureY));
            _log($"Touchpad: two-finger orbit {_gestureFrames} frames, travel x={_gestureX:F0} y={_gestureY:F0} pt, " +
                 $"axisMix={axisMix:F2}, release v=({_vx:F0},{_vy:F0}) pt/s");
        }
        _pair = null;
        // Momentum only when the fingers were still moving as they lifted.
        if (Math.Sqrt(_vx * _vx + _vy * _vy) < 60 || Environment.TickCount64 - _lastFrameTicks > 60) return;
        if (_momentum is null)
        {
            _momentum = _queue.CreateTimer();
            _momentum.Interval = TimeSpan.FromMilliseconds(16);
            _momentum.IsRepeating = true;
            _momentum.Tick += OnMomentum;
        }
        _momentumTicks = Environment.TickCount64;
        _momentum.Start();
    }

    private void OnMomentum(DispatcherQueueTimer timer, object args)
    {
        var now = Environment.TickCount64;
        var dt = Math.Clamp((now - _momentumTicks) / 1000.0, 0.001, 0.05);
        _momentumTicks = now;
        var decay = Math.Exp(-dt / MomentumTau);
        _vx *= decay;
        _vy *= decay;
        if (Math.Sqrt(_vx * _vx + _vy * _vy) < 20) { timer.Stop(); return; }
        _orbitPoints((float)(_vx * dt), (float)(_vy * dt));
    }

    private unsafe Device? DeviceFor(nint handle)
    {
        if (_devices.TryGetValue(handle, out var known)) return known;
        Device? device = null;
        try
        {
            uint size = 0;
            GetRawInputDeviceInfoW(handle, RidiPreparsedData, null, ref size);
            if (size == 0) return _devices[handle] = null;
            var preparsed = new byte[size];
            fixed (byte* p = preparsed)
            {
                if ((int)GetRawInputDeviceInfoW(handle, RidiPreparsedData, p, ref size) < 0) return _devices[handle] = null;
                var caps = new byte[64];
                fixed (byte* c = caps)
                    if (HidP_GetCaps(p, c) != HidpStatusSuccess) return _devices[handle] = null;
                var valueCount = BinaryPrimitives.ReadUInt16LittleEndian(caps.AsSpan(48)); // HIDP_CAPS.NumberInputValueCaps
                var values = new byte[Math.Max(1, (int)valueCount) * 72];
                ushort length = valueCount;
                fixed (byte* v = values)
                    if (HidP_GetValueCaps(0, v, ref length, p) != HidpStatusSuccess) return _devices[handle] = null;
                device = Describe(preparsed, values, length);
            }
            if (device is not null)
                _log($"Touchpad: device {handle:X} with {device.Fingers.Count} finger slots, " +
                     $"{device.WidthMillimetres:F0} × {device.HeightMillimetres:F0} mm, contact count {(device.ContactCountCollection is null ? "absent" : "present")}.");
        }
        catch (Exception error) { _log($"Touchpad: device {handle:X} not usable: {error.Message}"); }
        return _devices[handle] = device;
    }

    /// Finger collections from HIDP_VALUE_CAPS (72 bytes each): X/Y on page 1 per link collection.
    private static Device? Describe(byte[] preparsed, byte[] caps, int count)
    {
        var fingers = new Dictionary<ushort, Finger>();
        int minX = 0, maxX = 0, minY = 0, maxY = 0, physX = 0, physY = 0, unitExp = 0;
        uint units = 0;
        ushort? contactCount = null;
        for (var k = 0; k < count; k++)
        {
            var c = caps.AsSpan(k * 72, 72);
            var page = BinaryPrimitives.ReadUInt16LittleEndian(c);
            var collection = BinaryPrimitives.ReadUInt16LittleEndian(c[6..]);
            var isRange = c[12] != 0;
            var usage = BinaryPrimitives.ReadUInt16LittleEndian(c[56..]);
            var exp = (int)BinaryPrimitives.ReadUInt32LittleEndian(c[32..]);
            var unitCode = BinaryPrimitives.ReadUInt32LittleEndian(c[36..]);
            var logicalMin = BinaryPrimitives.ReadInt32LittleEndian(c[40..]);
            var logicalMax = BinaryPrimitives.ReadInt32LittleEndian(c[44..]);
            var physicalMax = BinaryPrimitives.ReadInt32LittleEndian(c[52..]);
            if (isRange) continue;
            if (page == 0x0D && usage == 0x54) contactCount = collection;
            if (page == 0x0D && usage == 0x51)
            {
                fingers.TryGetValue(collection, out var f);
                fingers[collection] = f with { Collection = collection, HasContactId = true };
            }
            if (page == 0x01 && usage is 0x30 or 0x31)
            {
                fingers.TryGetValue(collection, out var f);
                fingers[collection] = f with { Collection = collection, HasX = f.HasX || usage == 0x30, HasY = f.HasY || usage == 0x31 };
                if (usage == 0x30) { minX = logicalMin; maxX = logicalMax; physX = physicalMax; unitExp = exp; units = unitCode; }
                else { minY = logicalMin; maxY = logicalMax; physY = physicalMax; }
            }
        }
        var slots = fingers.Values.Where(f => f.HasX && f.HasY).OrderBy(f => f.Collection).ToList();
        if (slots.Count == 0 || maxX <= minX || maxY <= minY) return null;
        // Physical extents are in centimetres (unit 0x11) or inches (0x13) scaled by 10^exp; when
        // absent, assume a typical 105 × 65 mm pad so motion still feels right.
        var exponent = unitExp > 7 ? unitExp - 16 : unitExp;
        var scale = Math.Pow(10, exponent);
        var toMm = (units & 0xF) == 3 ? 25.4 : 10.0; // 0x13 = inches, 0x11 = centimetres
        double widthMm = physX > 0 ? physX * scale * toMm : 105, heightMm = physY > 0 ? physY * scale * toMm : 65;
        if (widthMm < 30 || widthMm > 400) widthMm = 105;
        if (heightMm < 20 || heightMm > 300) heightMm = 65;
        return new Device(preparsed, slots, contactCount, minX, minY,
            widthMm / (maxX - minX), heightMm / (maxY - minY), widthMm, heightMm);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _momentum?.Stop();
        var device = new RawInputDevice { UsagePage = 0x0D, Usage = 0x05, Flags = 0x00000001, Target = 0 }; // RIDEV_REMOVE
        RegisterRawInputDevices([device], 1, (uint)Marshal.SizeOf<RawInputDevice>());
        if (_subclassed) RemoveWindowSubclass(_window, _procedure, _subclassId);
        _subclassed = false;
    }

    private readonly record struct Contact(double X, double Y);
    private readonly record struct Finger(ushort Collection, bool HasX, bool HasY, bool HasContactId);
    private sealed record Device(byte[] Preparsed, List<Finger> Fingers, ushort? ContactCountCollection,
        int MinX, int MinY, double MillimetresPerUnitX, double MillimetresPerUnitY, double WidthMillimetres, double HeightMillimetres);

    [StructLayout(LayoutKind.Sequential)]
    private struct RawInputDevice { public ushort UsagePage, Usage; public uint Flags; public nint Target; }

    private delegate nint SubclassProcedure(nint hwnd, uint message, nuint wParam, nint lParam, nuint id, nuint data);

    [DllImport("user32.dll", SetLastError = true)] private static extern bool RegisterRawInputDevices(RawInputDevice[] devices, uint count, uint size);
    [DllImport("user32.dll")] private static extern unsafe uint GetRawInputData(nint handle, uint command, byte* data, ref uint size, uint headerSize);
    [DllImport("user32.dll")] private static extern unsafe uint GetRawInputDeviceInfoW(nint device, uint command, byte* data, ref uint size);
    [DllImport("comctl32.dll", SetLastError = true)] private static extern bool SetWindowSubclass(nint hwnd, SubclassProcedure procedure, nuint id, nuint data);
    [DllImport("comctl32.dll")] private static extern bool RemoveWindowSubclass(nint hwnd, SubclassProcedure procedure, nuint id);
    [DllImport("comctl32.dll")] private static extern nint DefSubclassProc(nint hwnd, uint message, nuint wParam, nint lParam);
    [DllImport("hid.dll")] private static extern unsafe int HidP_GetCaps(byte* preparsed, byte* caps);
    [DllImport("hid.dll")] private static extern unsafe int HidP_GetValueCaps(int reportType, byte* caps, ref ushort length, byte* preparsed);
    [DllImport("hid.dll")] private static extern unsafe int HidP_GetUsageValue(int reportType, ushort usagePage, ushort collection, ushort usage,
        out uint value, byte* preparsed, byte* report, uint length);
    [DllImport("hid.dll")] private static extern unsafe int HidP_GetUsages(int reportType, ushort usagePage, ushort collection, ushort* usages,
        ref uint length, byte* preparsed, byte* report, uint reportLength);
}
