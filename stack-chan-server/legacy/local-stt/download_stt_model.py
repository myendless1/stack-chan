#!/usr/bin/env python3
import argparse


def main():
    parser = argparse.ArgumentParser(description="Download a faster-whisper model into the local HF cache.")
    parser.add_argument("model", nargs="?", default="small")
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--compute-type", default="int8")
    args = parser.parse_args()

    from faster_whisper import WhisperModel

    WhisperModel(args.model, device=args.device, compute_type=args.compute_type)
    print(f"downloaded and initialized faster-whisper model: {args.model}")


if __name__ == "__main__":
    main()
