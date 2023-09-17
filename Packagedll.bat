"C:\MRE_SDK\tools\DllPackage.exe" "C:\Users\mmb\dev\mrv32\mrv32.vcproj"
if %errorlevel% == 0 (
 echo postbuild OK.
  copy mrv32.vpp ..\..\..\MoDIS_VC9\WIN32FS\DRIVE_E\mrv32.vpp /y
exit 0
)else (
echo postbuild error
  echo error code: %errorlevel%
  exit 1
)

