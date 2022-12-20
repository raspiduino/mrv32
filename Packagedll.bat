"C:\Program Files (x86)\MRE SDK V3.0.00\tools\DllPackage.exe" "D:\Data\dev\mrv32\mrv32.vcproj"
if %errorlevel% == 0 (
 echo postbuild OK.
  copy mrv32.vpp ..\..\..\MoDIS_VC9\WIN32FS\DRIVE_E\mrv32.vpp /y
exit 0
)else (
echo postbuild error
  echo error code: %errorlevel%
  exit 1
)

