#include "compiler/flags.h"

namespace chainer_compiler {

bool g_compiler_log;

bool g_permissive;

bool g_skip_inference;

bool g_replace_constant;

int g_recompute_relu;

bool g_modify_pool_with_imbalanced_pads;

bool g_use_cuda;

bool g_fuse_operations;

bool g_use_nvrtc;

bool g_use_tvm;

bool g_reuse_tvm_code;

std::string g_dump_autotvm_task_dir;

std::string g_autotvm_log;

std::string g_backend_name;

bool g_dump_after_inference;
bool g_dump_after_simplification;
bool g_dump_after_gradient;
bool g_dump_after_fusion;
bool g_dump_after_scheduling;
bool g_dump_subgraphs;

}  // namespace chainer_compiler
