#!/bin/bash

# Function to show usage
show_usage() {
    echo "Usage:"
    echo "  Split mode: $0 split <number_of_parts> <input_file>"
    echo "  Concat mode: $0 concat <output_file> <input_files...>"
    exit 1
}

# Check if enough arguments are provided
if [ $# -lt 3 ]; then
    show_usage
fi

mode=$1
shift

case $mode in
    "split")
        num_parts=$1
        input_file=$2
        
        # Check if input file exists
        if [ ! -f "$input_file" ]; then
            echo "Error: Input file does not exist"
            exit 1
        fi
        
        # Calculate total size and size per part
        total_size=$(stat -c %s "$input_file")
        size_per_part=$(( (total_size + num_parts - 1) / num_parts ))
        
        # Split the file
        split -b $size_per_part "$input_file" "${input_file}."
        echo "File split into $num_parts parts"
        ;;
        
    "concat")
        output_file=$1
        shift
        
        # Concatenate files
        cat "$@" > "$output_file"
        echo "Files concatenated into $output_file"
        ;;
        
    *)
        echo "Error: Invalid mode. Use 'split' or 'concat'"
        show_usage
        ;;
esac
