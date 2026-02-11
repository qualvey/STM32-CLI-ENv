# ==========================================
# 自动化编译烧录脚本 (run.ps1)
# ==========================================

# 1. 获取脚本所在的当前目录（绝对路径）
$ROOT_DIR = $PSScriptRoot
$BUILD_DIR = Join-Path $ROOT_DIR "build"
$ELF_FILE = Join-Path $BUILD_DIR "stm32_cli_test.elf"

# 2. 清理并准备构建目录
if (Test-Path $BUILD_DIR) {
    Write-Host "--- 清理旧构建目录 ---" -ForegroundColor Cyan
    Remove-Item -Recurse -Force $BUILD_DIR
}
New-Item -ItemType Directory -Path $BUILD_DIR | Out-Null

# 3. 切换到构建目录
Push-Location $BUILD_DIR

Write-Host "--- 开始配置项目 ---" -ForegroundColor Cyan
# 使用绝对路径指向父目录的 CMakeLists.txt
cmake .. -G "Unix Makefiles"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Error: CMake 配置失败！" -ForegroundColor Red
    Pop-Location; exit $LASTEXITCODE
}

Write-Host "--- 开始编译 ---" -ForegroundColor Cyan
make -j4  # -j4 代表开启 4 线程并行编译，速度更快
if ($LASTEXITCODE -ne 0) {
    Write-Host "Error: 编译失败！" -ForegroundColor Red
    Pop-Location; exit $LASTEXITCODE
}

# 4. 烧录检查
Write-Host "--- 准备烧录 ---" -ForegroundColor Green
if (Test-Path $ELF_FILE) {
    # 自动搜索 OpenOCD 路径（如果你把 OpenOCD 装在非标准位置）
    openocd -f interface/stlink.cfg -f target/stm32f1x.cfg `
            -c "program {$ELF_FILE} verify reset exit"
} else {
    Write-Host "Error: 找不到生成文件 $ELF_FILE" -ForegroundColor Red
}

# 5. 回到原始目录
Pop-Location
