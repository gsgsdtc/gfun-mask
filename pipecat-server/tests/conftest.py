"""
@doc     docs/modules/pipecat-pipeline/design/05-pipecat-server-refactor-backend-design.md §5
@purpose 将 pipecat-server/ 根目录添加到 sys.path，确保绝对导入正常工作
@context 测试文件移至 tests/ 子目录后，需显式添加父目录到 sys.path
"""

import sys
import os

# 将 pipecat-server/ 根目录加入 sys.path
sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
