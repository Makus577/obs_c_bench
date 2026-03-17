#!/usr/bin/env python3

import argparse
import os
import platform
import shutil
import subprocess
import urllib.request
import zipfile


SDK_ZIP_URL = "https://github.com/huaweicloud/huaweicloud-sdk-c-obs/archive/refs/heads/master.zip"


def run_command(command, cwd=None, env=None):
    print(f"Running: {command} in {cwd or os.getcwd()}")
    process = subprocess.Popen(
        command,
        shell=True,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if process.stdout:
        for line in process.stdout:
            print(line, end="")
    process.wait()
    if process.returncode != 0:
        raise RuntimeError(f"Command failed with return code {process.returncode}")


def first_existing(paths):
    for path in paths:
        if os.path.exists(path):
            return path
    return None


def detect_platform():
    system = platform.system().lower()
    machine = platform.machine().lower()
    return f"{system}-{machine}", machine


def ensure_clean_dir(path):
    if os.path.exists(path):
        shutil.rmtree(path)
    os.makedirs(path)


def main():
    parser = argparse.ArgumentParser(description="Download, build and stage OBS C SDK for this repo.")
    parser.add_argument(
        "--output-root",
        default=None,
        help="Directory where the built SDK should be staged. Defaults to .deps/obs_sdk/<platform>/",
    )
    args = parser.parse_args()

    base_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    platform_tag, machine = detect_platform()
    output_root = args.output_root or os.path.join(base_dir, ".deps", "obs_sdk", platform_tag)

    downloads_dir = os.path.join(base_dir, ".deps", "downloads")
    temp_extract_dir = os.path.join(base_dir, ".deps", "sdk_temp_extract")
    os.makedirs(downloads_dir, exist_ok=True)

    sdk_zip_path = os.path.join(downloads_dir, "obs_sdk_master.zip")
    print(f"Detected platform: {platform_tag}")
    print(f"OBS SDK output root: {output_root}")

    print(f"Downloading SDK from {SDK_ZIP_URL} ...")
    urllib.request.urlretrieve(SDK_ZIP_URL, sdk_zip_path)

    print("Unzipping SDK...")
    ensure_clean_dir(temp_extract_dir)
    with zipfile.ZipFile(sdk_zip_path, "r") as zip_ref:
        zip_ref.extractall(temp_extract_dir)

    extracted_items = os.listdir(temp_extract_dir)
    if not extracted_items:
        raise RuntimeError("Downloaded SDK archive is empty")

    actual_sdk_root = os.path.join(temp_extract_dir, extracted_items[0])
    build_dir = first_existing(
        [
            os.path.join(actual_sdk_root, "source/eSDK_OBS_API/eSDK_OBS_API_C++"),
            os.path.join(actual_sdk_root, "eSDK_OBS_API/eSDK_OBS_API_C++"),
        ]
    )
    if not build_dir:
        raise RuntimeError(f"Could not locate SDK build directory under {actual_sdk_root}")

    env = os.environ.copy()
    env["SPDLOG_VERSION"] = "spdlog-1.12.0"
    is_arm = "aarch64" in machine or machine.startswith("arm")
    build_cmd = "bash build_aarch.sh sdk" if is_arm else "bash build.sh sdk"

    print(f"Building SDK for {'ARM' if is_arm else 'x86'}...")
    run_command(build_cmd, cwd=build_dir, env=env)

    sdk_tgz = os.path.join(build_dir, "sdk.tgz")
    if not os.path.exists(sdk_tgz):
        raise RuntimeError(f"Expected built SDK archive not found: {sdk_tgz}")

    print("Extracting sdk.tgz...")
    run_command("tar zxvf sdk.tgz", cwd=build_dir)

    sdk_root = first_existing(
        [
            os.path.join(build_dir, "sdk"),
            build_dir,
        ]
    )
    include_src = first_existing(
        [
            os.path.join(sdk_root, "include"),
            os.path.join(build_dir, "include"),
            os.path.join(actual_sdk_root, "include"),
        ]
    )
    lib_src = first_existing(
        [
            os.path.join(sdk_root, "lib"),
            os.path.join(build_dir, "lib"),
        ]
    )

    if not include_src or not os.path.exists(os.path.join(include_src, "eSDKOBS.h")):
        raise RuntimeError("Could not locate built SDK headers (eSDKOBS.h)")
    if not lib_src or not os.listdir(lib_src):
        raise RuntimeError("Could not locate built SDK libraries")

    ensure_clean_dir(output_root)
    shutil.copytree(include_src, os.path.join(output_root, "include"))
    shutil.copytree(lib_src, os.path.join(output_root, "lib"))

    print("SDK bootstrap complete.")
    print(f"SDK root: {output_root}")
    print("Next steps:")
    print(f"  make")
    print(f"  OBS_SDK_ROOT={output_root} make")


if __name__ == "__main__":
    main()
