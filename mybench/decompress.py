#!/usr/bin/env python3
"""
ZST文件解压模块
功能：解压指定文件夹内所有.zst文件
"""

import os
import subprocess
import sys
from pathlib import Path

def decompress_zst_files(folder_path):
    """
    解压文件夹内所有.zst文件
    
    参数:
        folder_path (str): 包含.zst文件的文件夹路径
        
    返回:
        list: 成功解压的文件路径列表
    """
    zst_files = list(Path(folder_path).glob("*.zst"))
    
    if not zst_files:
        print(f"在文件夹 {folder_path} 中未找到任何.zst文件")
        return []
    
    decompressed_files = []
    
    for zst_file in zst_files:
        try:
            # 构建解压后的文件名（去掉.zst后缀）
            output_file = zst_file.with_suffix('')
            
            # 如果解压后的文件已存在，跳过解压
            if output_file.exists():
                print(f"文件已存在，跳过解压: {output_file.name}")
                decompressed_files.append(str(output_file))
                continue
            
            print(f"正在解压: {zst_file.name}")
            
            # 运行zstd解压命令
            result = subprocess.run(
                ['zstd', '-d', '--rm', str(zst_file)],  # 添加--rm参数，解压后删除原文件
                capture_output=True,
                text=True,
                check=True
            )
            
            if output_file.exists():
                decompressed_files.append(str(output_file))
                print(f"✓ 解压成功: {output_file.name}")
            else:
                print(f"✗ 解压后文件不存在: {output_file}")
                
        except subprocess.CalledProcessError as e:
            print(f"✗ 解压失败 {zst_file.name}: {e.stderr}")
        except Exception as e:
            print(f"✗ 解压错误 {zst_file.name}: {str(e)}")
    
    return decompressed_files

def main():
    """
    主函数 - 处理命令行参数并执行解压操作
    """
    if len(sys.argv) != 2:
        print("用法: python decompress_files.py <文件夹路径>")
        print("示例: python decompress_files.py ./data")
        sys.exit(1)
    
    folder_path = sys.argv[1]
    
    # 检查文件夹是否存在
    if not Path(folder_path).exists():
        print(f"错误: 文件夹不存在 {folder_path}")
        sys.exit(1)
    
    # 检查zstd命令是否可用
    try:
        subprocess.run(['zstd', '--version'], capture_output=True, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("错误: 系统未安装zstd工具，请先安装zstd")
        print("Ubuntu/Debian: sudo apt install zstd")
        print("CentOS/RHEL: sudo yum install zstd")
        print("macOS: brew install zstd")
        sys.exit(1)
    
    print("开始解压ZST文件...")
    print(f"数据目录: {folder_path}")
    print("-" * 50)
    
    # 执行解压操作
    decompressed_files = decompress_zst_files(folder_path)
    
    if not decompressed_files:
        print("没有找到可处理的文件")
        sys.exit(1)
    
    print(f"\n解压完成! 成功解压 {len(decompressed_files)} 个文件")

if __name__ == "__main__":
    main()