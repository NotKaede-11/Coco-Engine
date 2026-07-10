import os

def main():
    # Get the project root directory (parent of scripts/)
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    nnue_path = os.path.join(project_root, 'coco.nnue')
    header_path = os.path.join(project_root, 'src', 'nnue_data.h')
    
    if not os.path.exists(nnue_path):
        print(f"Error: {nnue_path} not found!")
        exit(1)
        
    print(f"Generating {header_path} from {nnue_path}...")
    with open(nnue_path, 'rb') as f:
        data = f.read()
        
    # Convert bytes to comma-separated hex values
    hex_data = ','.join(f'0x{b:02x}' for b in data)
    
    with open(header_path, 'w') as f:
        f.write(hex_data)
        
    print("Generation successful!")

if __name__ == '__main__':
    main()
