using System.IO.MemoryMappedFiles;
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("Nebula.Embed.Cli")]

namespace Nebula.Embed;

/// <summary>Read-only tensor pages; leases keep an old mapping alive across index replacement.</summary>
internal sealed unsafe class ReadOnlyTensorMap<T> : IDisposable where T : unmanaged
{
    private readonly object _gate = new();
    private MemoryMappedFile? _file;
    private MemoryMappedViewAccessor? _view;
    private T* _data;
    private bool _pointerHeld, _ownerReleased;
    private int _references = 1;
    public int Rows { get; }
    public int Columns { get; }

    public ReadOnlyTensorMap(string path, int rows, int columns)
    {
        if (!BitConverter.IsLittleEndian) throw new PlatformNotSupportedException("Tensor packs are little-endian.");
        if (rows <= 0 || columns <= 0) throw new ArgumentOutOfRangeException(nameof(rows));
        var elements = checked(rows * columns);
        var bytes = checked((long)elements * sizeof(T));
        Rows = rows; Columns = columns;
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
        if (stream.Length != bytes) throw new InvalidDataException($"{path}: expected {bytes} bytes, found {stream.Length}.");
        try
        {
            _file = MemoryMappedFile.CreateFromFile(stream, null, 0, MemoryMappedFileAccess.Read, HandleInheritability.None, leaveOpen: true);
            _view = _file.CreateViewAccessor(0, bytes, MemoryMappedFileAccess.Read);
            byte* pointer = null;
            _view.SafeMemoryMappedViewHandle.AcquirePointer(ref pointer);
            _pointerHeld = true;
            _data = (T*)(pointer + _view.PointerOffset);
        }
        catch { Close(); throw; }
    }

    public Lease Acquire()
    {
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ownerReleased, this);
            _references++;
            return new Lease(this);
        }
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_ownerReleased) return;
            _ownerReleased = true;
            Release();
        }
    }

    private void Release()
    {
        lock (_gate)
        {
            if (--_references != 0) return;
            Close();
            GC.SuppressFinalize(this);
        }
    }

    private void Close()
    {
        if (_pointerHeld) { _view!.SafeMemoryMappedViewHandle.ReleasePointer(); _pointerHeld = false; }
        _data = null;
        _view?.Dispose(); _file?.Dispose();
        _view = null; _file = null;
    }

    ~ReadOnlyTensorMap() => Close();

    internal sealed class Lease : IDisposable
    {
        private ReadOnlyTensorMap<T>? _owner;
        internal Lease(ReadOnlyTensorMap<T> owner) => _owner = owner;
        public ReadOnlySpan<T> Row(int index)
        {
            var owner = _owner ?? throw new ObjectDisposedException(nameof(Lease));
            if ((uint)index >= owner.Rows) throw new ArgumentOutOfRangeException(nameof(index));
            return new ReadOnlySpan<T>(owner._data + (long)index * owner.Columns, owner.Columns);
        }
        public void Dispose() => Interlocked.Exchange(ref _owner, null)?.Release();
    }
}
