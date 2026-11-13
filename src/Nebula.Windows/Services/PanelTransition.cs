namespace Nebula_Windows.Services;

/// <summary>
/// One sampled geometry/opacity state for BOTH XAML and its native glass mask.
/// No compositor animation runs independently of these values.
/// </summary>
internal readonly record struct PanelFrame(double X, double Y, double Width, double Height, double Opacity)
{
    public static PanelFrame Interpolate(PanelFrame from, PanelFrame to, double amount) => new(
        from.X + (to.X - from.X) * amount,
        from.Y + (to.Y - from.Y) * amount,
        from.Width + (to.Width - from.Width) * amount,
        from.Height + (to.Height - from.Height) * amount,
        from.Opacity + (to.Opacity - from.Opacity) * amount);
}

/// <summary>A bounded, retargetable 200 ms transition, sampled only by the page's shared clock.</summary>
internal sealed class PanelTransition(PanelFrame initial, bool isVisible)
{
    public const double DurationMilliseconds = 200;
    private PanelFrame _from = initial;
    private double _startedAt;
    public PanelFrame Current { get; private set; } = initial;
    public PanelFrame Target { get; private set; } = initial;
    public bool IsVisibleTarget { get; private set; } = isVisible;
    public bool IsRunning { get; private set; }

    public void Retarget(PanelFrame target, bool isVisible, double now, bool animate)
    {
        Sample(now);
        if (Target == target && IsVisibleTarget == isVisible)
        {
            if (!animate) Complete();
            return; // Repeated LayoutChrome calls do not restart an existing deadline.
        }
        _from = Current;
        Target = target;
        IsVisibleTarget = isVisible;
        _startedAt = now;
        IsRunning = animate && Current != Target;
        if (!IsRunning) Complete();
    }

    public PanelFrame Sample(double now)
    {
        if (!IsRunning) return Current;
        var progress = Math.Clamp((now - _startedAt) / DurationMilliseconds, 0, 1);
        if (progress >= 1) { Complete(); return Current; }
        // Smoothstep: zero endpoint velocity, no overshoot of opacity, geometry or list viewport.
        var eased = progress * progress * (3 - 2 * progress);
        Current = PanelFrame.Interpolate(_from, Target, eased);
        return Current;
    }

    public void Complete()
    {
        Current = Target;
        IsRunning = false;
    }
}
