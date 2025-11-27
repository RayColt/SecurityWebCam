# SecurityWebCam
In Development Security WebCam with Hotspot Selection
<img src=https://github.com/RayColt/SecurityWebCam/blob/master/.gitfiles/swc.jpg />
<br><br>
Win64 UI + OpenCV tracker + auto motion init + save per second<br>
Build with >= C++17, link with OpenCV, user32, gdi32, ole32<br>
Win64 + DirectShow enumeration (Unicode) + OpenCV capture (CAP_DSHOW) + TrackerCSRT<br>
Notes: Requires OpenCV contrib (tracking module) present in vcpkg opencv4 port.<br>
.\vcpkg remove opencv4:x64-windows<br>
.\vcpkg install opencv4[contrib]:x64-windows<br>