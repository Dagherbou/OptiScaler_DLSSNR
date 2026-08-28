import io

# --- the supersampled enlargement's runtime refusal degrades instead of latching ---
p = 'OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

old = """    if (!srOk)
    {
        LOG_ERROR("DLSS-NR split: the enlargement failed; falling back");
        SplitDx12.failed = true;
        DlssNr::SetSplitActive(false);
        DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
    }"""
assert old in t
new = """    if (!srOk && supersample && !SplitDx12.supersampleRefused)
    {
        // The driver refused the supersampled enlargement at evaluate. Run at display size from the
        // next frame instead of latching the whole split off; the enlargement is rebuilt for the new
        // target by the srTargetWidth check above.
        SplitDx12.supersampleRefused = true;
        LOG_ERROR("DLSS-NR split: the supersampled enlargement refused to run; dropping to display "
                  "size");
        DlssNr::SetSplitStatus("supersample refused here; enlargement at display size");
    }
    else if (!srOk)
    {
        LOG_ERROR("DLSS-NR split: the enlargement failed; falling back");
        SplitDx12.failed = true;
        DlssNr::SetSplitActive(false);
        DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
    }"""
t = t.replace(old, new, 1)

# --- Retry clears the refusal too ---
old = """static void SplitClearFailure()
{
    SplitDx12.failed = false;
    SplitDx12.armTries = 0;"""
assert old in t
t = t.replace(old, """static void SplitClearFailure()
{
    SplitDx12.failed = false;
    SplitDx12.armTries = 0;
    SplitDx12.supersampleRefused = false;""", 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('runtime refusal fallback in')
