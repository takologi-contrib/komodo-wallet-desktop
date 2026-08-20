Set-ExecutionPolicy RemoteSigned -scope CurrentUser

$Env:QT_INSTALL_CMAKE_PATH = "C:\Qt\$Env:QT_VERSION\msvc2019_64"
$Env:QT_ROOT = "C:\Qt"

git clone https://github.com/cipig/coins/ -b nogeo
mkdir -p atomic_defi_design\assets\images\coins
Get-Item -Path "coins\icons\*.png" | Move-Item -Destination "atomic_defi_design\assets\images\coins"

mkdir b
cd b

Invoke-Expression "cmake -DCMAKE_BUILD_TYPE=$Env:CMAKE_BUILD_TYPE -DDEX_SHOW_COMMIT_HASH=ON -GNinja ../"
ninja install
