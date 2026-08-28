"""Tells the user about the Neural Rendering DLL at the end of the installer.

The pass needs nvngx_dlssnr.dll, which cannot be shipped -- it comes from an NVIDIA driver package. So
the installer has to say where it goes, and it may as well look and say whether it is already there,
since that is the single most common reason for the feature to sit silently disabled.

It goes in this same folder: the one holding the game's executable, alongside OptiScaler itself. One
copy per game, the same as everything else here.

Written as a file rather than an inline heredoc because batch escaping and shell escaping disagree about
carets, percent signs and parentheses.
"""

import io

PATH = "C:/Games_Temp/OptiScaler/setup_windows.bat"
text = io.open(PATH, encoding="utf-8", errors="surrogateescape").read()

OLD = """set setupSuccess=true

:end
pause"""

NEW = """set setupSuccess=true

REM --- DLSS 5 Neural Rendering -------------------------------------------------------------------
REM This build adds a detail-synthesis pass that runs on NVIDIA's Neural Rendering model. The model
REM itself ships in a driver package and cannot be redistributed here, so the user has to supply it.
echo.
echo  ------------------------------------------------------------------
echo   DLSS 5 Neural Rendering
echo  ------------------------------------------------------------------
echo.
if exist "nvngx_dlssnr.dll" (
    echo   nvngx_dlssnr.dll found in this folder. Neural Rendering can run.
) else (
    echo   nvngx_dlssnr.dll was NOT found in this folder.
    echo.
    echo   Neural Rendering needs it, and it cannot be shipped with OptiScaler
    echo   because it comes from an NVIDIA driver package. Copy it into THIS
    echo   folder - the same one holding the game executable and the file
    echo   OptiScaler was just renamed to.
    echo.
    echo   One copy per game. There is no shared or system-wide location.
)
echo.
echo   Neural Rendering is OFF by default. Turn it on in the OptiScaler
echo   overlay under "DLSS Neural Rendering", or set Enabled=true in the
echo   section named DlssNr in OptiScaler.ini.
echo.
echo   It needs an RTX 50 series card and a driver new enough to ship the
echo   model. If it cannot run, the overlay says why rather than failing
echo   quietly.
echo.

:end
pause"""

assert OLD in text, "installer tail not found"
text = text.replace(OLD, NEW, 1)

io.open(PATH, "w", encoding="utf-8", errors="surrogateescape", newline='\r\n').write(text)
print("setup_windows.bat patched")
