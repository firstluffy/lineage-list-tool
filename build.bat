@echo off
rem 사용법: 더블클릭 또는  build.bat
rem         마지막 "아무 키나..." 없이 끝내려면  build.bat nopause
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "BUILD_DIR=build"
set "CONFIG=Release"

echo CMake 구성: "%BUILD_DIR%"
cmake -S . -B "%BUILD_DIR%"
if errorlevel 1 (
  echo.
  echo [오류] CMake 구성에 실패했습니다. Visual Studio ^(CMake 지원^) 또는 Build Tools가 설치되어 있는지 확인하세요.
  if /I not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo 빌드 중: %CONFIG%
cmake --build "%BUILD_DIR%" --config %CONFIG%
if errorlevel 1 (
  echo.
  echo [오류] 빌드에 실패했습니다.
  if /I not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo 빌드 완료.
echo 실행 파일: "%CD%\%BUILD_DIR%\%CONFIG%\변신리스트수정 툴.exe"
if /I not "%~1"=="nopause" pause
endlocal
