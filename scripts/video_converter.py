#!/usr/bin/env python3
import cv2
import os
import argparse
import struct
import numpy as np

def convert_video_to_frames(video_path, output_dir, quality=80, resolution=None, max_frames=None):
    """
    Convert a video file to JPEG frames and generate metadata
    """
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    # Open the video file
    cap = cv2.VideoCapture(video_path)
    
    if not cap.isOpened():
        print(f"Error: Could not open video file {video_path}")
        return False
    
    # Get video properties
    frame_count = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    fps = cap.get(cv2.CAP_PROP_FPS)
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    
    if max_frames and max_frames < frame_count:
        frame_count = max_frames
    
    print(f"Video: {video_path}")
    print(f"Frames: {frame_count}")
    print(f"FPS: {fps}")
    print(f"Resolution: {width}x{height}")
    
    # If resolution is specified, use it
    if resolution:
        try:
            width, height = map(int, resolution.split('x'))
        except:
            print(f"Invalid resolution format: {resolution}. Using original resolution.")
    
    # Prepare output files
    frames_file_path = os.path.join(output_dir, "video_frames.bin")
    metadata_file_path = os.path.join(output_dir, "video_metadata.bin")
    
    # Open binary files for writing
    with open(frames_file_path, 'wb') as frames_file, open(metadata_file_path, 'wb') as metadata_file:
        # Write number of frames to metadata file
        metadata_file.write(struct.pack('<I', frame_count))
        
        # Process each frame
        frame_number = 0
        frame_sizes = []
        
        while True:
            ret, frame = cap.read()
            if not ret or (max_frames and frame_number >= max_frames):
                break
            
            # Resize if needed
            if resolution:
                frame = cv2.resize(frame, (width, height))
            
            # Convert frame to JPEG
            encode_param = [int(cv2.IMWRITE_JPEG_QUALITY), quality]
            _, jpeg_data = cv2.imencode('.jpg', frame, encode_param)
            
            # Write JPEG data to frames file
            frame_size = len(jpeg_data)
            frames_file.write(jpeg_data.tobytes())
            
            # Store frame size for metadata
            frame_sizes.append(frame_size)
            
            frame_number += 1
            if frame_number % 10 == 0:
                print(f"Processed {frame_number}/{frame_count} frames")
        
        # Write frame sizes to metadata file
        for size in frame_sizes:
            metadata_file.write(struct.pack('<I', size))
    
    # Calculate total size
    total_size = sum(frame_sizes)
    print(f"Total frames processed: {frame_number}")
    print(f"Total size: {total_size / 1024:.2f} KB")
    
    cap.release()
    return True

def main():
    parser = argparse.ArgumentParser(description='Convert video to JPEG frames for ESP32-S3 RTSP server')
    parser.add_argument('video_file', help='Path to the input video file')
    parser.add_argument('--output', '-o', default='data', help='Output directory (default: data)')
    parser.add_argument('--quality', '-q', type=int, default=80, help='JPEG quality (1-100, default: 80)')
    parser.add_argument('--resolution', '-r', help='Output resolution (WIDTHxHEIGHT, e.g. 640x480)')
    parser.add_argument('--max-frames', '-m', type=int, help='Maximum number of frames to process')
    
    args = parser.parse_args()
    
    convert_video_to_frames(
        args.video_file, 
        args.output, 
        quality=args.quality, 
        resolution=args.resolution,
        max_frames=args.max_frames
    )

if __name__ == "__main__":
    main()