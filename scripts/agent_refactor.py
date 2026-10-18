import sys

def refactor(filepath):
    with open(filepath, 'r') as f:
        lines = f.readlines()

    start_idx = -1
    end_idx = -1
    for i, line in enumerate(lines):
        if 'GlobalCommandRegistry::Clear();' in line:
            start_idx = i
        if 'std::unique_ptr<AI> ai;' in line:
            # We want to go back to the previous line that is '  });' or just empty lines
            for j in range(i-1, 0, -1):
                if '});' in lines[j]:
                    end_idx = j
                    break
            break
            
    if start_idx == -1 or end_idx == -1:
        print("Could not find GlobalCommandRegistry block: start={}, end={}".format(start_idx, end_idx))
        return

    # Extract block
    block = lines[start_idx:end_idx+1]
    
    # Delete block
    del lines[start_idx:end_idx+1]
    
    # Find LoadMap(mapPath);
    load_map_idx = -1
    for i, line in enumerate(lines):
        if 'LoadMap(mapPath);' in line:
            load_map_idx = i
            break
            
    if load_map_idx == -1:
        print("Could not find LoadMap(mapPath);")
        return
        
    # Insert block
    lines = lines[:load_map_idx] + block + ['\n'] + lines[load_map_idx:]
    
    # Delete createNPC Pietro
    pietro_idx = -1
    for i, line in enumerate(lines):
        if 'createNPC({20, 1}, "Pietro",' in line:
            pietro_idx = i
            break
            
    if pietro_idx != -1:
        # Delete this line and the next
        del lines[pietro_idx:pietro_idx+2]
    
    with open(filepath, 'w') as f:
        f.writelines(lines)
        
    print("Refactored src/GameECS.cpp successfully")

if __name__ == '__main__':
    refactor('src/GameECS.cpp')
