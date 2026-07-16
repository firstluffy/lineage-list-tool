@echo off
rem 배포용: MSVC Release + 정적 런타임(/MT) → VC++ 재배포 DLL 없이 단일 exe
rem 사용법: build_dist.bat  |  끝 대기 없음: build_dist.bat nopause
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "BUILD_DIR=build_dist"
set "CONFIG=Release"
set "OUTDIR=dist"

echo [배포] CMake 구성: "%BUILD_DIR%" ^(정적 런타임 ON^)
cmake -S . -B "%BUILD_DIR%" -DLINEAGE_LIST_STATIC_RUNTIME=ON
if errorlevel 1 (
  echo.
  echo [오류] CMake 구성 실패. Visual Studio ^(CMake^) 또는 Build Tools + MSVC가 필요합니다.
  if /I not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo [배포] 빌드: %CONFIG%
cmake --build "%BUILD_DIR%" --config %CONFIG%
if errorlevel 1 (
  echo.
  echo [오류] 빌드 실패. 실행 중인 변신리스트수정 툴.exe가 있으면 종료 후 다시 시도하세요.
  if /I not "%~1"=="nopause" pause
  exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
copy /Y "%BUILD_DIR%\%CONFIG%\변신리스트수정 툴.exe" "%OUTDIR%\변신리스트수정 툴.exe" >nul

echo.
echo 배포 완료.
echo 단일 exe: "%CD%\%OUTDIR%\변신리스트수정 툴.exe"
echo ^(RichEdit용 Msftedit.dll은 Windows에 기본 포함^)
if /I not "%~1"=="nopause" pause
endlocal
