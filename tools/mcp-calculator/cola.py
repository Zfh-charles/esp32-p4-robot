# test_env.py
import sys
import platform

print(f"=== Python 环境验证 ===")
print(f"Python版本: {platform.python_version()}")
print(f"解释器路径: {sys.executable}")
print(f"平台: {platform.platform()}")
print(f"命令行参数: {sys.argv}")
print("========================")