#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (c) 2026 SZTU Open Source Organization.
# 应用项目管理脚本：创建/删除/启用/禁用应用，自动维护 3 个配置文件
#
# 适用平台：九联星闪开发板，基于海思 WS63E 芯片。
#
# 注意：applications/docs/scripts/make_app.py 为本脚本副本，两处修改后需同步更新。
#
# 用法见 -h。主脚本：在源码根目录执行 python3 applications/make_app.py；在 applications/ 下可执行 python3 make_app.py
# 创建: ... -c <项目目录名> [目标名]
# 删除: ... --delete <项目目录名> [目标名]
# 禁用: ... -x <项目目录名> [目标名]
# 启用: ... -e <项目目录名> [目标名]
# 列表: ... -l  编译: ... -b  全量编译: ... -f  帮助: ... [-h]
#

import os
import re
import shutil
import subprocess
import sys
from typing import Optional, Tuple

# 默认产品名（九联星闪开发板）
DEFAULT_PRODUCT = "nearlink_dk_3863"

# 源码根目录：主脚本在 applications/；applications/docs/scripts/ 副本需上溯三级
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_parent = os.path.dirname(SCRIPT_DIR)
_grandparent = os.path.dirname(_parent)
if os.path.basename(SCRIPT_DIR) == "scripts" and os.path.basename(_parent) == "docs":
    if os.path.basename(_grandparent) == "applications":
        ROOT_DIR = os.path.dirname(_grandparent)
    else:
        ROOT_DIR = _grandparent
elif os.path.basename(SCRIPT_DIR) == "applications":
    ROOT_DIR = _parent
else:
    ROOT_DIR = SCRIPT_DIR

# 配置文件路径
APP_BUILD_GN = os.path.join(ROOT_DIR, "applications/sample/wifi-iot/app/BUILD.gn")
CONFIG_PY = os.path.join(ROOT_DIR, "device/soc/hisilicon/ws63v100/sdk/build/config/target_config/ws63/config.py")
OHOS_CMAKE = os.path.join(ROOT_DIR, "device/soc/hisilicon/ws63v100/sdk/libs_url/ws63/cmake/ohos.cmake")


def _script_rel_to_root() -> str:
    """相对源码根目录的脚本路径，用于帮助与错误提示。"""
    return os.path.relpath(os.path.abspath(__file__), ROOT_DIR).replace("\\", "/")


def _usage_command_line() -> str:
    """帮助首行：仅在 argv 为 make_app.py / ./make_app.py 时显示简写（通常当前目录为 applications/）。"""
    av0 = sys.argv[0].replace("\\", "/")
    if av0 in ("make_app.py", "./make_app.py"):
        return "python3 make_app.py"
    return f"python3 {_script_rel_to_root()}"


def _usage_alternate_line() -> str:
    """与首行相对的另一种启动方式说明（仅主脚本 applications/make_app.py）。"""
    rel = _script_rel_to_root()
    if rel != "applications/make_app.py":
        return ""
    av0 = sys.argv[0].replace("\\", "/")
    if av0 in ("make_app.py", "./make_app.py"):
        return "（在 OpenHarmony 源码根目录请使用: python3 applications/make_app.py [选项] [参数...]）"
    if av0 == "applications/make_app.py" or av0.endswith("/applications/make_app.py"):
        return "（在 applications 目录下可简写: python3 make_app.py [选项] [参数...]）"
    return ""


# 项目模板（基于九联星闪开发板，海思 WS63E 芯片）
C_TEMPLATE = '''/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>

#include "ohos_init.h"

static void {entry_name}(void)
{{
    printf("Hello, {project_name}!\\r\\n");
}}

SYS_RUN({entry_name});
'''

BUILD_GN_TEMPLATE = '''# Copyright (c) 2026 SZTU Open Source Organization.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

static_library("{target_name}") {{
  sources = [ "{source_file}" ]

  include_dirs = [
    "//commonlibrary/utils_lite/include",
  ]
}}
'''


def _confirm(prompt: str, default_no: bool = True) -> bool:
    """二次确认，default_no=True 时回车视为 N"""
    suffix = " (y/N): " if default_no else " (Y/n): "
    try:
        ans = input(prompt + suffix).strip().lower()
    except (EOFError, KeyboardInterrupt):
        return False
    if not ans:
        return not default_no
    return ans in ("y", "yes")


def create_project_files(project_dir: str, target_name: str) -> bool:
    """创建项目目录和模板文件。若目录已存在则返回 False（由调用方决定是否仅添加配置）"""
    app_dir = os.path.join(ROOT_DIR, "applications/sample/wifi-iot/app")
    project_path = os.path.join(app_dir, project_dir)

    if os.path.exists(project_path):
        return False

    os.makedirs(project_path, exist_ok=True)

    # 生成入口函数名（如 my_demo -> MyDemoEntry）
    parts = project_dir.replace("-", "_").split("_")
    entry_name = "".join(p.capitalize() for p in parts) + "Entry"
    source_file = f"{project_dir}.c"

    # 写入 .c 文件
    c_content = C_TEMPLATE.format(
        entry_name=entry_name,
        project_name=project_dir.replace("_", " ").title()
    )
    c_path = os.path.join(project_path, source_file)
    with open(c_path, "w", encoding="utf-8") as f:
        f.write(c_content)
    print(f"[创建] {c_path}")

    # 写入 BUILD.gn
    build_gn_content = BUILD_GN_TEMPLATE.format(
        target_name=target_name,
        source_file=source_file
    )
    build_gn_path = os.path.join(project_path, "BUILD.gn")
    with open(build_gn_path, "w", encoding="utf-8") as f:
        f.write(build_gn_content)
    print(f"[创建] {build_gn_path}")

    return True


def _strip_line_comment(line: str) -> str:
    """去掉行内注释（# 及之后），GN 和 Python 均以 # 开始注释"""
    idx = line.find("#")
    if idx >= 0:
        return line[:idx].strip()
    return line.strip()


def _feature_in_effective_build_gn(content: str, feature_entry: str) -> bool:
    """检查 feature 是否已存在于 BUILD.gn 的有效内容中（排除注释）"""
    pattern = r'features = \[\s*(.*?)\s*\]'
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        return False
    block = match.group(1)
    for line in block.split("\n"):
        active = _strip_line_comment(line)
        if active and feature_entry in active:
            return True
    return False


def _deduplicate_build_gn_features(content: str, feature_entry: str) -> str:
    """移除 features 块中重复的 feature_entry 行"""
    pattern = r'(features = \[\s*)(.*?)(\s*\])'
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        return content
    block = match.group(2)
    seen_feature = False
    new_lines = []
    for line in block.split("\n"):
        active = _strip_line_comment(line)
        if active and feature_entry in active:
            if seen_feature:
                continue  # 跳过重复行
            seen_feature = True
        new_lines.append(line)
    new_block = "\n".join(new_lines)
    return content[:match.start(2)] + new_block + content[match.start(3):]


def modify_app_build_gn(project_dir: str, target_name: str) -> bool:
    """修改 app/BUILD.gn"""
    if not os.path.exists(APP_BUILD_GN):
        print(f"[错误] 文件不存在: {APP_BUILD_GN}")
        return False

    with open(APP_BUILD_GN, "r", encoding="utf-8") as f:
        content = f.read()

    feature_entry = f'"{project_dir}:{target_name}"'
    if _feature_in_effective_build_gn(content, feature_entry):
        print(f"[跳过] app/BUILD.gn 已包含 {feature_entry}")
        return True

    # 若在注释中，则取消注释（不新增行）
    uncomment_pattern = (
        r'^(\s*)#\s*(' + re.escape(feature_entry) + r'\s*,?\s*)(\s*(?:#.*)?)$'
    )
    new_content = re.sub(uncomment_pattern, r'\1\2\3', content, flags=re.MULTILINE)
    if new_content != content:
        # 取消注释后可能产生重复行，需去重
        new_content = _deduplicate_build_gn_features(new_content, feature_entry)
        with open(APP_BUILD_GN, "w", encoding="utf-8") as f:
            f.write(new_content)
        print(f"[修改] {APP_BUILD_GN} - 取消注释 {feature_entry}")
        return True

    # 在最后一个 feature 后添加新项（在 ] 前），确保逗号正确
    pattern = r'(features = \[\s*)(.*?)(\s*\])'
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        print("[错误] 无法解析 app/BUILD.gn 的 features 结构")
        return False

    block = match.group(2).rstrip()
    # 若上一项不以逗号结尾，则补上（GN 要求项与项之间必须有逗号）
    if block and not re.search(r',\s*$', block):
        block += ','
    new_block = block + f'\n    {feature_entry},\n'
    new_content = content[:match.start(2)] + new_block + content[match.start(3):]

    with open(APP_BUILD_GN, "w", encoding="utf-8") as f:
        f.write(new_content)
    print(f"[修改] {APP_BUILD_GN} - 添加 {feature_entry}")
    return True


def modify_config_py(target_name: str) -> bool:
    """修改 config.py"""
    if not os.path.exists(CONFIG_PY):
        print(f"[错误] 文件不存在: {CONFIG_PY}")
        return False

    with open(CONFIG_PY, "r", encoding="utf-8") as f:
        content = f.read()

    if f'"{target_name}"' in content and "'ws63-liteos-app'" in content:
        # 检查是否在 ram_component 中
        if re.search(rf'["\']{target_name}["\']\s*,?\s*\n\s*[\'"]xo_trim_port', content):
            print(f"[跳过] config.py 已包含 {target_name}")
            return True

    # 在 'xo_trim_port' 前插入
    pattern = r"(\s+)('xo_trim_port',)"
    replacement = rf'\1"{target_name}",\n\1\2'
    new_content = re.sub(pattern, replacement, content, count=1)

    if new_content == content:
        print("[错误] 无法在 config.py 中找到插入位置")
        return False

    with open(CONFIG_PY, "w", encoding="utf-8") as f:
        f.write(new_content)
    print(f"[修改] {CONFIG_PY} - 添加 {target_name}")
    return True


def modify_ohos_cmake(target_name: str) -> bool:
    """修改 ohos.cmake"""
    if not os.path.exists(OHOS_CMAKE):
        print(f"[错误] 文件不存在: {OHOS_CMAKE}")
        return False

    with open(OHOS_CMAKE, "r", encoding="utf-8") as f:
        content = f.read()

    if f'"{target_name}"' in content:
        print(f"[跳过] ohos.cmake 已包含 {target_name}")
        return True

    # 在 ws63-liteos-app 块的 COMPONENT_LIST 最后一个 ")" 前添加新目标
    # 匹配 ws63-liteos-app 块中最后一个 "xxx") 的格式
    pattern = r'(elseif\(\$\{TARGET_COMMAND\} MATCHES "ws63-liteos-app"\)\s+set\(COMPONENT_LIST\s+)(.*?)(\s*\)\s*\nendif)'
    match = re.search(pattern, content, re.DOTALL)
    if match:
        # 在最后一个 ) 前添加 "target_name"
        block = match.group(2)
        # 最后一个 ) 是 set 的闭合，block 末尾可能有换行和空格
        # 在 block 末尾的 ) 前插入，block 实际是 "item1" "item2" ... 多行
        # 找到 block 内最后一个 "xxx") 模式并添加
        new_block = block.rstrip()
        # 在末尾添加 "target_name"，格式为 "last")\n -> "last" "target")\n
        new_block = new_block + f' "{target_name}"'
        new_content = content[:match.start(2)] + new_block + content[match.start(3):]
        with open(OHOS_CMAKE, "w", encoding="utf-8") as f:
            f.write(new_content)
        print(f"[修改] {OHOS_CMAKE} - 添加 {target_name}")
        return True

    print("[错误] 无法解析 ohos.cmake 的 COMPONENT_LIST 结构")
    return False


def get_target_from_build_gn(project_path: str) -> str | None:
    """从项目目录的 BUILD.gn 中解析 static_library 目标名"""
    build_gn = os.path.join(project_path, "BUILD.gn")
    if not os.path.exists(build_gn):
        return None
    with open(build_gn, "r", encoding="utf-8") as f:
        content = f.read()
    m = re.search(r'static_library\s*\(\s*["\']([^"\']+)["\']', content)
    return m.group(1) if m else None


def remove_app_build_gn(project_dir: str, target_name: str) -> bool:
    """从 app/BUILD.gn 中移除 feature"""
    if not os.path.exists(APP_BUILD_GN):
        print(f"[错误] 文件不存在: {APP_BUILD_GN}")
        return False

    with open(APP_BUILD_GN, "r", encoding="utf-8") as f:
        content = f.read()

    feature_entry = f'"{project_dir}:{target_name}"'
    if feature_entry not in content:
        print(f"[跳过] app/BUILD.gn 中未找到 {feature_entry}")
        return True

    # 移除整行（含逗号、换行）
    pattern = re.escape(feature_entry) + r'\s*,?\s*\n\s*'
    new_content = re.sub(pattern, "", content)
    # 处理可能产生的多余逗号：上一项末尾的逗号后直接是 ] 时，移除该逗号
    new_content = re.sub(r',\s*\n(\s+)\]', r'\n\1]', new_content)
    # 处理可能产生的连续逗号（如删除后留下 ,,）
    new_content = re.sub(r',\s*,', ',', new_content)
    with open(APP_BUILD_GN, "w", encoding="utf-8") as f:
        f.write(new_content)
    print(f"[修改] {APP_BUILD_GN} - 移除 {feature_entry}")
    return True


def remove_config_py(target_name: str) -> bool:
    """从 config.py 的 ram_component 中移除目标"""
    if not os.path.exists(CONFIG_PY):
        print(f"[错误] 文件不存在: {CONFIG_PY}")
        return False

    with open(CONFIG_PY, "r", encoding="utf-8") as f:
        content = f.read()

    # 移除 "target_name", 或 "target_name"\n 整行（含前后空白行）
    pattern = rf'\s*["\']{re.escape(target_name)}["\']\s*,?\s*\n'
    new_content = re.sub(pattern, "\n", content)
    if new_content == content:
        print(f"[跳过] config.py 中未找到 {target_name}")
        return True

    with open(CONFIG_PY, "w", encoding="utf-8") as f:
        f.write(new_content)
    print(f"[修改] {CONFIG_PY} - 移除 {target_name}")
    return True


def remove_ohos_cmake(target_name: str) -> bool:
    """从 ohos.cmake 的 COMPONENT_LIST 中移除目标"""
    if not os.path.exists(OHOS_CMAKE):
        print(f"[错误] 文件不存在: {OHOS_CMAKE}")
        return False

    with open(OHOS_CMAKE, "r", encoding="utf-8") as f:
        content = f.read()

    if f'"{target_name}"' not in content:
        print(f"[跳过] ohos.cmake 中未找到 {target_name}")
        return True

    # 在 ws63-liteos-app 块中移除 "target_name"
    pattern = r'(elseif\(\$\{TARGET_COMMAND\} MATCHES "ws63-liteos-app"\)\s+set\(COMPONENT_LIST\s+)(.*?)(\s*\)\s*\nendif)'
    match = re.search(pattern, content, re.DOTALL)
    if match:
        block = match.group(2)
        # 移除 "target_name"（保留原有格式）
        new_block = block.replace(f' "{target_name}"', '')
        if new_block == block:
            new_block = block.replace(f'"{target_name}"', '')
        new_content = content[:match.start(2)] + new_block + content[match.start(3):]
        with open(OHOS_CMAKE, "w", encoding="utf-8") as f:
            f.write(new_content)
        print(f"[修改] {OHOS_CMAKE} - 移除 {target_name}")
        return True

    print("[错误] 无法解析 ohos.cmake 的 COMPONENT_LIST 结构")
    return False


def disable_project(project_dir: str, target_name: Optional[str]) -> bool:
    """禁用项目：从 3 个配置中移除，不编译不打包，保留项目目录和源码"""
    app_dir = os.path.join(ROOT_DIR, "applications/sample/wifi-iot/app")
    project_path = os.path.join(app_dir, project_dir)

    if target_name is None:
        if os.path.exists(project_path):
            target_name = get_target_from_build_gn(project_path)
        if target_name is None:
            print("[错误] 无法获取目标名，请指定: --disable <项目目录名> <目标名>")
            return False

    if not os.path.exists(project_path):
        print(f"[错误] 项目目录不存在: {project_path}")
        return False

    if not remove_app_build_gn(project_dir, target_name):
        return False
    if not remove_config_py(target_name):
        return False
    if not remove_ohos_cmake(target_name):
        return False
    return True


def enable_project(project_dir: str, target_name: Optional[str]) -> bool:
    """启用项目：在 3 个配置中添加，参与编译和打包进固件"""
    app_dir = os.path.join(ROOT_DIR, "applications/sample/wifi-iot/app")
    project_path = os.path.join(app_dir, project_dir)

    if target_name is None:
        if os.path.exists(project_path):
            target_name = get_target_from_build_gn(project_path)
        if target_name is None:
            print("[错误] 无法获取目标名，请指定: --enable <项目目录名> <目标名>")
            return False

    if not os.path.exists(project_path):
        print(f"[错误] 项目目录不存在: {project_path}")
        return False

    if not modify_app_build_gn(project_dir, target_name):
        return False
    if not modify_config_py(target_name):
        return False
    if not modify_ohos_cmake(target_name):
        return False
    return True


def delete_project(project_dir: str, target_name: Optional[str]) -> bool:
    """删除项目：移除目录并从 3 个配置中移除"""
    app_dir = os.path.join(ROOT_DIR, "applications/sample/wifi-iot/app")
    project_path = os.path.join(app_dir, project_dir)

    if target_name is None:
        if os.path.exists(project_path):
            target_name = get_target_from_build_gn(project_path)
        if target_name is None:
            print("[错误] 无法获取目标名，请指定: --delete <项目目录名> <目标名>")
            return False

    # 1. 删除项目目录
    if os.path.exists(project_path):
        shutil.rmtree(project_path)
        print(f"[删除] {project_path}")
    else:
        print(f"[跳过] 项目目录不存在: {project_path}")

    # 2. 从 3 个配置中移除
    if not remove_app_build_gn(project_dir, target_name):
        return False
    if not remove_config_py(target_name):
        return False
    if not remove_ohos_cmake(target_name):
        return False
    return True


def list_apps() -> bool:
    """显示当前应用列表：扫描 app 目录下所有项目，并标注是否已启用（参与编译和打包）"""
    app_dir = os.path.join(ROOT_DIR, "applications/sample/wifi-iot/app")
    if not os.path.isdir(app_dir):
        print(f"[错误] 目录不存在: {app_dir}")
        return False

    # 读取 app/BUILD.gn 中的 features（排除注释行及行内注释后的内容）
    build_gn_features: set = set()
    if os.path.exists(APP_BUILD_GN):
        with open(APP_BUILD_GN, "r", encoding="utf-8") as f:
            for line in f:
                line = _strip_line_comment(line)
                if not line or line.startswith("#"):
                    continue
                m = re.search(r'"([^"]+):([^"]+)"', line)
                if m:
                    build_gn_features.add((m.group(1), m.group(2)))

    # 读取 config.py 中 ws63-liteos-app 的 ram_component 列表（排除注释中的目标名）
    config_py_targets: set = set()
    if os.path.exists(CONFIG_PY):
        with open(CONFIG_PY, "r", encoding="utf-8") as f:
            content = f.read()
        match = re.search(
            r"'ws63-liteos-app'\s*:\s*\{.*?'ram_component'\s*:\s*\[(.*?)\],\s*'ccflags'",
            content, re.DOTALL
        )
        if match:
            block = match.group(1)
            for raw_line in block.split("\n"):
                line = _strip_line_comment(raw_line)
                if not line or line.startswith("#"):
                    continue
                for m in re.finditer(r'["\']([a-zA-Z][a-zA-Z0-9_]*)["\']', line):
                    config_py_targets.add(m.group(1))

    # 扫描 app 目录下的子目录（含 BUILD.gn 的视为项目）
    projects: list = []
    for name in sorted(os.listdir(app_dir)):
        path = os.path.join(app_dir, name)
        if not os.path.isdir(path) or name.startswith("."):
            continue
        build_gn = os.path.join(path, "BUILD.gn")
        if not os.path.isfile(build_gn):
            continue
        target = get_target_from_build_gn(path)
        if not target:
            continue
        in_build = (name, target) in build_gn_features
        in_config = target in config_py_targets
        enabled = in_build and in_config
        projects.append((name, target, enabled))

    if not projects:
        print("当前无应用项目。")
        return True

    print("当前应用列表 (applications/sample/wifi-iot/app/):")
    print("-" * 56)
    print(f"{'项目目录':<20} {'目标名':<20} {'状态':<10}")
    print("-" * 56)
    for proj_dir, target, enabled in projects:
        status = "启用" if enabled else "禁用"
        print(f"{proj_dir:<20} {target:<20} {status:<10}")
    print("-" * 56)
    print(f"共 {len(projects)} 个项目，启用 {sum(1 for _, _, e in projects if e)} 个")
    return True


def build_project(full: bool = True) -> bool:
    """执行编译：hb set -p <product> 后 hb build [-f]"""
    os.chdir(ROOT_DIR)
    cmd_set = ["hb", "set", "-p", DEFAULT_PRODUCT]
    cmd_build = ["hb", "build", "-f"] if full else ["hb", "build"]
    print(f"[编译] 产品: {DEFAULT_PRODUCT}")
    print("-" * 50)
    if subprocess.run(cmd_set, cwd=ROOT_DIR).returncode != 0:
        print("[错误] hb set 失败")
        return False
    if subprocess.run(cmd_build, cwd=ROOT_DIR).returncode != 0:
        print("[错误] hb build 失败")
        return False
    print("-" * 50)
    print("编译完成!")
    return True


def build_project_full() -> bool:
    """全量编译：rm -rf out 后 hb set -p <product> && hb build -f"""
    out_dir = os.path.join(ROOT_DIR, "out")
    os.chdir(ROOT_DIR)
    if os.path.isdir(out_dir):
        print(f"[全量编译] 清理: rm -rf {out_dir}")
        shutil.rmtree(out_dir)
    print("-" * 50)
    return build_project(full=True)


def _validate_project_args(args: list, opt: str) -> Tuple[str, Optional[str]]:
    """校验项目目录名和目标名，返回 (project_dir, target_name)。args[0]=脚本名, args[1]=--opt"""
    if len(args) < 3:
        print(f"用法: python3 {_script_rel_to_root()} {opt} <项目目录名> [目标名]")
        sys.exit(1)
    project_dir = args[2].strip()
    target_name = args[3].strip() if len(args) > 3 else None
    if not re.match(r'^[a-zA-Z][a-zA-Z0-9_-]*$', project_dir):
        print("[错误] 项目目录名只能包含字母、数字、下划线和连字符")
        sys.exit(1)
    if target_name is not None and not re.match(r'^[a-zA-Z][a-zA-Z0-9_]*$', target_name):
        print("[错误] 目标名只能包含字母、数字和下划线")
        sys.exit(1)
    return project_dir, target_name


def print_help() -> None:
    """打印帮助信息"""
    cmd = _usage_command_line()
    script = _script_rel_to_root()
    print("用法:", cmd, "[选项] [参数...]")
    alt = _usage_alternate_line()
    if alt:
        print("    ", alt)
    print()
    print("选项:")
    print("  -h, --help    显示此帮助信息（不带参数时默认显示）")
    print("  -c, --create  新建项目（若目录已存在可择添加至配置）")
    print("  --delete      删除项目（需二次确认）")
    print("  -x, --disable 禁用项目（不编译不打包，保留目录和源码）")
    print("  -e, --enable  启用项目（参与编译和打包进固件）")
    print("  -l, --list    查看当前应用列表及启用状态")
    print("  -b, --build   执行编译（hb set -p nearlink_dk_3863 && hb build -f）")
    print("  -f, --full    全量编译（rm -rf out 后重新编译）")
    print()
    print("示例:")
    print("  python3", script, "-c my_demo")
    print("  python3", script, "-c led_blink led_blink")
    print("  python3", script, "--delete my_demo")
    print("  python3", script, "-x gpio_led")
    print("  python3", script, "-e gpio_led")
    print("  python3", script, "-l")
    print("  python3", script, "-b")
    print("  python3", script, "-f")


# 选项简写映射：简写 -> 规范名（用于内部判断和错误提示）
_OPT_ALIASES = {
    "-c": "--create", "--create": "--create",
    "-e": "--enable", "--enable": "--enable",
    "-x": "--disable", "--disable": "--disable",
    "-l": "--list", "--list": "--list",
    "-b": "--build", "--build": "--build",
    "-f": "--full", "--full": "--full",
}


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("--help", "-h", "-?"):
        print_help()
        sys.exit(0)

    opt_raw = args[0].lower()
    opt = _OPT_ALIASES.get(opt_raw, opt_raw)
    if opt not in ("--create", "--delete", "--disable", "--enable", "--list", "--build", "--full"):
        print(f"[错误] 未知选项: {args[0]}")
        print("使用 -h 或 --help 查看用法")
        sys.exit(1)

    os.chdir(ROOT_DIR)

    # 列表模式
    if opt == "--list":
        list_apps()
        return

    # 编译模式
    if opt == "--build":
        if not build_project():
            sys.exit(1)
        return

    # 全量编译模式
    if opt == "--full":
        if not build_project_full():
            sys.exit(1)
        return

    # 禁用模式
    if opt == "--disable":
        project_dir, target_name = _validate_project_args(sys.argv, "-x")
        print(f"禁用项目: 目录={project_dir}" + (f", 目标={target_name}" if target_name else " (自动解析)"))
        print("-" * 50)
        if not disable_project(project_dir, target_name):
            sys.exit(1)
        print("-" * 50)
        print("禁用完成! 该应用将不再参与编译和打包。")
        return

    # 启用模式
    if opt == "--enable":
        project_dir, target_name = _validate_project_args(sys.argv, "-e")
        print(f"启用项目: 目录={project_dir}" + (f", 目标={target_name}" if target_name else " (自动解析)"))
        print("-" * 50)
        if not enable_project(project_dir, target_name):
            sys.exit(1)
        print("-" * 50)
        print("启用完成! 请执行 hb build -f 重新编译。")
        return

    # 删除模式（仅 --delete，无简写）
    if opt == "--delete":
        project_dir, target_name = _validate_project_args(sys.argv, "--delete")
        app_dir = os.path.join(ROOT_DIR, "applications/sample/wifi-iot/app")
        project_path = os.path.join(app_dir, project_dir)
        if target_name is None and os.path.exists(project_path):
            target_name = get_target_from_build_gn(project_path)
        if not _confirm(f"确定要删除项目 {project_dir} 吗？此操作不可恢复"):
            print("已取消删除。")
            sys.exit(0)
        print(f"删除项目: 目录={project_dir}" + (f", 目标={target_name}" if target_name else " (自动解析)"))
        print("-" * 50)
        if not delete_project(project_dir, target_name):
            sys.exit(1)
        print("-" * 50)
        print("删除完成!")
        return

    # 创建模式
    if opt == "--create":
        if len(args) < 2:
            print(f"用法: python3 {_script_rel_to_root()} -c <项目目录名> [目标名]")
            sys.exit(1)
        project_dir = args[1].strip()
        target_name = args[2].strip() if len(args) > 2 else f"{project_dir}_demo"

        if not re.match(r'^[a-zA-Z][a-zA-Z0-9_-]*$', project_dir):
            print("[错误] 项目目录名只能包含字母、数字、下划线和连字符")
            sys.exit(1)

        if not re.match(r'^[a-zA-Z][a-zA-Z0-9_]*$', target_name):
            print("[错误] 目标名只能包含字母、数字和下划线")
            sys.exit(1)

        app_dir = os.path.join(ROOT_DIR, "applications/sample/wifi-iot/app")
        project_path = os.path.join(app_dir, project_dir)

        if not create_project_files(project_dir, target_name):
            # 项目目录已存在
            if not os.path.isdir(project_path):
                sys.exit(1)
            exist_target = get_target_from_build_gn(project_path)
            if not exist_target:
                print(f"[错误] 项目目录已存在但无法解析目标名: {project_path}")
                print("请检查 BUILD.gn 或使用 -e 启用已有项目。")
                sys.exit(1)
            if not _confirm(f"项目目录 {project_dir} 已存在，是否将其添加到配置中参与编译？"):
                print("已取消。使用 -e 可启用已有项目。")
                sys.exit(0)
            target_name = exist_target
            print(f"将已有项目添加到配置: 目录={project_dir}, 目标={target_name}")
        else:
            print(f"创建新项目: 目录={project_dir}, 目标={target_name}")

        print("-" * 50)
        if not modify_app_build_gn(project_dir, target_name):
            sys.exit(1)
        if not modify_config_py(target_name):
            sys.exit(1)
        if not modify_ohos_cmake(target_name):
            sys.exit(1)

        print("-" * 50)
        print("完成! 请执行以下命令编译:")
        print("  hb set -p nearlink_dk_3863")
        print("  hb build -f")
        print(f"\n项目路径: applications/sample/wifi-iot/app/{project_dir}/")


if __name__ == "__main__":
    main()
