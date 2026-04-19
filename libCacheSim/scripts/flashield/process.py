import os
import re

def parse_log_line(line):
    """
    解析log文件的最后一行，提取miss ratio和括号中的write_byte比例
    """
    # 提取miss ratio
    miss_ratio_match = re.search(r'miss ratio:\s*([\d.]+)', line)
    miss_ratio = miss_ratio_match.group(1) if miss_ratio_match else "N/A"
    
    # 提取括号中的数字，如(0.1018)
    write_byte_match = re.search(r'\((\d+\.\d+)\)\s*$', line)
    write_byte = write_byte_match.group(1) if write_byte_match else "N/A"
    
    return miss_ratio, write_byte

def process_logs():
    """
    处理三个文件夹中的所有log文件
    """
    folders = ['results/0.01/dram0.1', 'results/0.01/dram0.01', 'results/0.01/dram0.001']
    dram_ratios = {'dram0.1': '0.1', 'dram0.01': '0.01', 'dram0.001': '0.001'}
    
    results = []
    
    for folder in folders:
        # 从文件夹名提取dram_ratio
        folder_name = os.path.basename(folder)
        dram_ratio = dram_ratios.get(folder_name, 'unknown')
        
        # 检查文件夹是否存在
        if not os.path.exists(folder):
            print(f"警告: 文件夹 {folder} 不存在，跳过...")
            continue
        
        # 遍历文件夹中的所有.log文件
        for filename in os.listdir(folder):
            if filename.endswith('.log'):
                filepath = os.path.join(folder, filename)
                
                try:
                    # 读取文件的最后一行
                    with open(filepath, 'r', encoding='utf-8') as f:
                        lines = f.readlines()
                        if lines:
                            last_line = lines[-1].strip()
                            
                            # 解析最后一行
                            miss_ratio, write_byte = parse_log_line(last_line)
                            
                            # 去掉.log后缀
                            log_name = filename[:-4]
                            
                            # 添加到结果列表
                            results.append({
                                'log_name': log_name,
                                'dram_ratio': dram_ratio,
                                'miss_ratio': miss_ratio,
                                'write_byte': write_byte
                            })
                            
                            print(f"已处理: {filepath}")
                        else:
                            print(f"警告: {filepath} 是空文件")
                            
                except Exception as e:
                    print(f"错误: 处理文件 {filepath} 时出错: {e}")
    
    return results

def write_results(results, output_file='output.txt'):
    """
    将结果写入txt文件
    """
    with open(output_file, 'w', encoding='utf-8') as f:
        # 写入表头
        f.write("log_name, dram_ratio, miss_ratio, write_byte\n")
        
        # 写入数据
        for item in results:
            f.write(f"{item['log_name']}, {item['dram_ratio']}, {item['miss_ratio']}, {item['write_byte']}\n")
    
    print(f"\n结果已保存到 {output_file}")
    print(f"共处理 {len(results)} 个log文件")

if __name__ == "__main__":
    # 处理所有log文件
    results = process_logs()
    
    # 将结果写入文件
    if results:
        write_results(results)
    else:
        print("没有找到任何log文件或所有文件都处理失败")