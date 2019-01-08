#include "compiler/xcvm/emitter.h"

#include <map>

#include <common/log.h>
#include <common/strutil.h>
#include <compiler/flags.h>
#include <compiler/graph.h>
#include <compiler/log.h>
#include <compiler/model.h>
#include <compiler/node.h>
#include <compiler/nvrtc_builder.h>
#include <compiler/passes.h>
#include <compiler/tvm/compiler.h>
#include <compiler/value.h>
#include <runtime/gen_xcvm_codegen.h>
#include <runtime/xcvm.pb.h>

namespace oniku {
namespace xcvm {
namespace {

#define FREE(...)                                                                                         \
    do {                                                                                                  \
        AddFreeOp(prog, __VA_ARGS__);                                                                     \
        prog->mutable_instructions(prog->instructions_size() - 1)->set_debug_info(StrCat("@", __LINE__)); \
    } while (0)

#define MOVE(dst, src)            \
    do {                          \
        EMIT(Identity, dst, src); \
        FREE(src);                \
    } while (0)

using oniku::runtime::XCProgramProto;

std::vector<int> IntVector(const std::vector<int64_t>& ints) {
    return std::vector<int>{ints.begin(), ints.end()};
}

void FillOpInfo(const Node& node, const std::string& debug_info, XCProgramProto* prog) {
    runtime::XCInstructionProto* inst = prog->mutable_instructions(prog->instructions_size() - 1);
    inst->set_debug_info(debug_info);
    inst->set_id(node.onikux_order());
}

class XCVMEmitter {
public:
    XCVMEmitter() {
    }

    void EmitModel(const Graph& graph, XCProgramProto* program, bool dump_value_names) {
        AssignValueIds(graph);
        EmitGraph(graph, program, false /* in_loop */, graph.output_values());
        EmitOutputs(graph.output_values(), program);
        if (dump_value_names) {
            std::map<int, const Value*> values;
            for (auto p : value_ids_) {
                values.emplace(p.second, p.first);
            }
            std::cerr << "=== " << values.size() << " variables ===\n";
            int64_t total = 0;
            for (auto p : values) {
                const Value* v = p.second;
                int64_t size = v->GetNBytes();
                total += size;
                std::cerr << "$" << p.first << ": " << v->name() << ' ' << size << std::endl;
            }
            int64_t total_mb = total / 1000 / 1000;
            std::cerr << "Total size of all values: " << total_mb << "MB" << std::endl;
        }
        EmitStackQuit(program);
    }

    void AssignValueIds(const std::set<Value*>& values) {
        for (const Value* v : values) {
            CHECK(value_ids_.emplace(v, next_value_id_++).second);
        }
    }

    void EmitNodes(const std::vector<Node*>& nodes, runtime::XCProgramProto* program) {
        for (Node* node : nodes) {
            EmitNode(nullptr /* graph */, *node, program);
        }
    }

    int GetValueId(const Value* v) const {
        CHECK(!v->name().empty()) << v->DebugString();
        auto found = value_ids_.find(v);
        CHECK(found != value_ids_.end()) << "Value not exist: " << v->name();
        return found->second;
    }

private:
    void AssignValueIds(const Graph& graph) {
        for (const Value* v : graph.input_values()) {
            CHECK(value_ids_.emplace(v, next_value_id_++).second) << v->DebugString();
        }
        for (const Value* v : graph.temp_values()) {
            CHECK(value_ids_.emplace(v, next_value_id_++).second) << v->DebugString();
        }
        for (const Value* v : graph.output_values()) {
            // We allow graph output to be null.
            // TODO(hamaji): Revisit this design. Probably, it would
            // be better to mark outputs are unnecessary instead of
            // using null values.
            CHECK(value_ids_.emplace(v, next_value_id_++).second || v->name().empty()) << v->DebugString();
        }
    }

    int GetStackId(int i) const {
        auto found = stack_ids_.find(i);
        CHECK(found != stack_ids_.end()) << "Stack not exist: " << i;
        return found->second;
    }

    void EmitStackQuit(XCProgramProto* prog) {
        for (auto p : stack_ids_) {
            FREE(p.second);
        }
    }

    void EmitNode(const Graph* graph, const Node& node, XCProgramProto* prog) {
        auto in = [this, &node](int i) {
            CHECK_LT(i, node.inputs().size()) << i << "th input of " << node.op_type() << " is mandatory";
            Value* input = node.inputs()[i];
            CHECK(!input->IsNull()) << i << "th input of " << node.op_type() << " is mandatory";
            return GetValueId(input);
        };

        // Optional input.
        auto oin = [this, in, &node](int i) {
            if (i >= static_cast<int>(node.inputs().size())) return -1;
            if (node.inputs()[i]->IsNull()) return -1;
            return in(i);
        };

        auto out = [this, &node](int i) {
            CHECK_LT(i, node.outputs().size()) << i << "th output of " << node.op_type() << " is mandatory";
            ;
            Value* output = node.outputs()[i];
            CHECK(!output->IsNull()) << i << "th output of " << node.op_type() << " is mandatory";
            return GetValueId(output);
        };

        // Optional output.
        auto oout = [this, out, &node](int i) {
            if (i >= static_cast<int>(node.outputs().size())) return -1;
            if (node.outputs()[i]->IsNull()) return -1;
            return out(i);
        };

        auto pads = [&node]() {
            std::vector<int> pads = IntVector(node.pads());
            if (pads.empty()) {
                pads = {0, 0};
            } else {
                // Both Chainer and xChainer expect paddings for beginning
                // and end are the same.
                CHECK_EQ(pads.size() % 2, 0);
                for (size_t i = 0; i < pads.size() / 2; ++i) {
                    CHECK_EQ(pads[i], pads[i + pads.size() / 2]);
                }
                pads.resize(pads.size() / 2);
            }
            return pads;
        };

        auto strides = [&node]() {
            std::vector<int> strides = IntVector(node.strides());
            // TODO(hamaji): Infer strides for non-2D convolutions/pools.
            if (strides.empty()) strides = {1, 1};
            return strides;
        };

        auto direction = [&node]() {
            const std::string& dir = node.direction();
            if (dir == "" || dir == "forward")
                return 0;
            else if (dir == "reverse")
                return 1;
            else if (dir == "bidirectional")
                return 2;
            else
                CHECK(false) << "Unknown direction: " << dir;
        };

#define EMIT(op, ...)                            \
    do {                                         \
        Add##op##Op(prog, __VA_ARGS__);          \
        FillOpInfo(node, node.ToString(), prog); \
    } while (0);

#define EMIT_SIMPLE_UNARY_OP(name, sym)           \
    do {                                          \
        if (node.op_type() == name) {             \
            CHECK_EQ(1UL, node.inputs().size());  \
            CHECK_EQ(1UL, node.outputs().size()); \
            EMIT(sym, out(0), in(0));             \
            return;                               \
        }                                         \
    } while (0)

#define EMIT_SIMPLE_BINARY_OP(name, sym)          \
    do {                                          \
        if (node.op_type() == name) {             \
            CHECK_EQ(2UL, node.inputs().size());  \
            CHECK_EQ(1UL, node.outputs().size()); \
            EMIT(sym, out(0), in(0), in(1));      \
            return;                               \
        }                                         \
    } while (0)

        EMIT_SIMPLE_UNARY_OP(Node::kNeg, Neg);
        EMIT_SIMPLE_UNARY_OP(Node::kReciprocal, Reciprocal);
        EMIT_SIMPLE_UNARY_OP(Node::kExp, Exp);
        EMIT_SIMPLE_UNARY_OP(Node::kLog, Log);
        EMIT_SIMPLE_UNARY_OP(Node::kSqrt, Sqrt);
        EMIT_SIMPLE_UNARY_OP(Node::kTanh, Tanh);
        EMIT_SIMPLE_UNARY_OP(Node::kAbs, Abs);
        EMIT_SIMPLE_UNARY_OP(Node::kRelu, Relu);
        EMIT_SIMPLE_UNARY_OP(Node::kFloor, Floor);
        EMIT_SIMPLE_UNARY_OP(Node::kCeil, Ceil);
        EMIT_SIMPLE_UNARY_OP(Node::kSigmoid, Sigmoid);
        EMIT_SIMPLE_UNARY_OP(Node::kNot, Not);
        EMIT_SIMPLE_UNARY_OP(Node::kIdentity, Identity);

        EMIT_SIMPLE_BINARY_OP(Node::kAdd, Add);
        EMIT_SIMPLE_BINARY_OP(Node::kSub, Sub);
        EMIT_SIMPLE_BINARY_OP(Node::kMul, Mul);
        EMIT_SIMPLE_BINARY_OP(Node::kDiv, Div);
        EMIT_SIMPLE_BINARY_OP(Node::kPow, Pow);
        EMIT_SIMPLE_BINARY_OP(Node::kEqual, Equal);
        EMIT_SIMPLE_BINARY_OP(Node::kGreater, Greater);
        EMIT_SIMPLE_BINARY_OP(Node::kOnikuxGenericIs, GenericIs);
        EMIT_SIMPLE_BINARY_OP(Node::kAnd, And);
        EMIT_SIMPLE_BINARY_OP(Node::kOr, Or);
        EMIT_SIMPLE_BINARY_OP(Node::kXor, Xor);

        EMIT_SIMPLE_BINARY_OP(Node::kOnikuxReluGrad, ReluGrad);
        EMIT_SIMPLE_BINARY_OP(Node::kOnikuxMaxPoolGrad, MaxPoolGrad);
        EMIT_SIMPLE_BINARY_OP(Node::kOnikuxAveragePoolGrad, AveragePoolGrad);
        EMIT_SIMPLE_BINARY_OP(Node::kOnikuxSelectItem, SelectItem);

        if (node.op_type() == Node::kDropout) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_LE(1UL, node.outputs().size());
            CHECK_GE(2UL, node.outputs().size());
            if (node.outputs().size() >= 2UL) {
                WARN_ONCE("The second output of Dropout is not handled yet");
            }
            EMIT(Dropout, out(0), oout(1), in(0), node.ratio());
        } else if (node.op_type() == Node::kSelu) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_LE(1UL, node.outputs().size());
            EMIT(Selu, out(0), in(0), node.alpha(), node.gamma());
        } else if (node.op_type() == Node::kLeakyRelu) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_LE(1UL, node.outputs().size());
            EMIT(LeakyRelu, out(0), in(0), node.alpha());
        } else if (node.op_type() == Node::kElu) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_LE(1UL, node.outputs().size());
            EMIT(Elu, out(0), in(0), node.alpha());
        } else if (node.op_type() == Node::kOnikuxLinear) {
            EMIT(Linear, out(0), in(0), in(1), oin(2), node.n_batch_axes());
        } else if (node.op_type() == Node::kOnikuxLinearGradWeight) {
            EMIT(LinearGradWeight, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kConv) {
            CHECK_LE(2UL, node.inputs().size());
            CHECK_GE(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            // TODO(xchainer): Support dilation.
            for (int d : node.dilations()) CHECK_EQ(d, 1) << "Dilation is not supported yet";
            EMIT(Conv, out(0), in(0), in(1), oin(2), strides(), pads());
        } else if (node.op_type() == Node::kConvTranspose) {
            CHECK_LE(2UL, node.inputs().size());
            CHECK_GE(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            // TODO(xchainer): Support dilation.
            for (int d : node.dilations()) CHECK_EQ(d, 1) << "Dilation is not supported yet";
            // TODO(hamaji): Handle output_padding and output_shape.
            std::vector<int> output_shape(IntVector(node.output_shape()));
            EMIT(ConvTranspose, out(0), in(0), in(1), oin(2), strides(), pads(), output_shape);
        } else if (node.op_type() == Node::kOnikuxConvTransposeWithDynamicOutputShape) {
            CHECK_EQ(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ConvTransposeWithDynamicShape, out(0), in(0), in(1), in(2), strides(), pads());
        } else if (node.op_type() == Node::kOnikuxConvGradWeight) {
            CHECK_EQ(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            // TODO(xchainer): Support dilation.
            for (int d : node.dilations()) CHECK_EQ(d, 1) << "Dilation is not supported yet";
            EMIT(ConvGradWeight, out(0), in(0), in(1), in(2), strides(), pads());
        } else if (node.op_type() == Node::kRNN) {
            CHECK(node.activations().empty()) << "activations not supporte yet";
            CHECK(node.activation_alpha().empty()) << "activation_alpha not supporte yet";
            CHECK(node.activation_beta().empty()) << "activation_beta not supporte yet";
            EMIT(RNN, oout(0), oout(1), in(0), in(1), in(2), oin(3), oin(4), oin(5), node.hidden_size(), direction());
        } else if (node.op_type() == Node::kGRU) {
            CHECK(node.activations().empty()) << "activations not supporte yet";
            CHECK(node.activation_alpha().empty()) << "activation_alpha not supporte yet";
            CHECK(node.activation_beta().empty()) << "activation_beta not supporte yet";
            EMIT(GRU,
                 oout(0),
                 oout(1),
                 in(0),
                 in(1),
                 in(2),
                 oin(3),
                 oin(4),
                 oin(5),
                 node.hidden_size(),
                 node.linear_before_reset(),
                 direction());
        } else if (node.op_type() == Node::kLSTM) {
            CHECK(node.activations().empty()) << "activations not supporte yet";
            CHECK(node.activation_alpha().empty()) << "activation_alpha not supporte yet";
            CHECK(node.activation_beta().empty()) << "activation_beta not supporte yet";
            EMIT(LSTM,
                 oout(0),
                 oout(1),
                 oout(2),
                 oout(3),
                 in(0),
                 in(1),
                 in(2),
                 oin(3),
                 oin(4),
                 oin(5),
                 oin(6),
                 oin(7),
                 node.hidden_size(),
                 direction());
        } else if (node.op_type() == Node::kOnikuxLSTMGrad) {
            EMIT(LSTMGrad, out(0), out(1), out(2), out(3), in(0), in(1));
        } else if (node.op_type() == Node::kShape) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Shape, out(0), in(0));
        } else if (node.op_type() == Node::kSize) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Size, out(0), in(0));
        } else if (node.op_type() == Node::kReshape) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Reshape, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kExpand) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Expand, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kSqueeze) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Squeeze, out(0), in(0), IntVector(node.axes()));
        } else if (node.op_type() == Node::kUnsqueeze) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Unsqueeze, out(0), in(0), IntVector(node.axes()));
        } else if (node.op_type() == Node::kMatMul) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(MatMul, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kGemm) {
            CHECK_EQ(3UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Gemm, out(0), in(0), in(1), in(2), node.alpha(), node.beta(), node.trans_a(), node.trans_b());
        } else if (node.op_type() == Node::kBatchNormalization) {
            CHECK_EQ(5UL, node.inputs().size());
            size_t num_onnx_outputs = node.outputs().size();
            std::vector<int> outs = {out(0)};
            if (node.outputs().back()->type().kind() == Type::Kind::kOpaque) {
                num_onnx_outputs--;
                outs.push_back(out(num_onnx_outputs));
            } else {
                outs.push_back(-1);
            }
            for (size_t i = 1; i < num_onnx_outputs; ++i) {
                outs.push_back(out(i));
            }
            for (size_t i = num_onnx_outputs; i < 6; ++i) {
                outs.push_back(-1);
            }

            EMIT(BatchNormalization,
                 outs[0],
                 outs[1],
                 outs[2],
                 outs[3],
                 outs[4],
                 outs[5],
                 in(0),
                 in(1),
                 in(2),
                 in(3),
                 in(4),
                 node.epsilon(),
                 node.momentum(),
                 node.spatial());
        } else if (node.op_type() == Node::kLRN) {
            if (node.outputs().size() == 1) {
                int tmp_id = next_value_id_++;
                EMIT(LRN, out(0), tmp_id, in(0), node.alpha(), node.beta(), node.bias(), node.size());
                FREE(tmp_id);
            } else {
                EMIT(LRN, out(0), out(1), in(0), node.alpha(), node.beta(), node.bias(), node.size());
            }
        } else if (node.op_type() == Node::kOnikuxLRNGrad) {
            EMIT(LRNGrad, out(0), in(0), in(1), in(2), in(3), node.alpha(), node.beta(), node.bias(), node.size());
        } else if (node.op_type() == Node::kPad) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            CHECK_EQ("constant", node.mode()) << "Only constant padding is supported";
            EMIT(Pad, out(0), in(0), IntVector(node.pads()), node.value());
        } else if (node.op_type() == Node::kMaxPool) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ("NOTSET", node.auto_pad()) << "auto_pad is not supported for MaxPool";
            if (node.outputs().size() == 1) {
                int tmp_id = next_value_id_++;
                EMIT(MaxPool, out(0), tmp_id, in(0), IntVector(node.kernel_shape()), strides(), pads(), node.onikux_cover_all());
                FREE(tmp_id);
            } else {
                CHECK_EQ(3UL, node.outputs().size());
                CHECK(node.outputs()[1]->IsNull());
                EMIT(MaxPool, out(0), out(2), in(0), IntVector(node.kernel_shape()), strides(), pads(), node.onikux_cover_all());
            }
        } else if (node.op_type() == Node::kAveragePool) {
            CHECK_EQ("NOTSET", node.auto_pad()) << "auto_pad is not supported for AveragePool";
            CHECK_EQ(1UL, node.inputs().size());
            if (node.outputs().size() == 1) {
                int tmp_id = next_value_id_++;
                EMIT(AveragePool, out(0), tmp_id, in(0), IntVector(node.kernel_shape()), strides(), pads(), node.count_include_pad());
                FREE(tmp_id);
            } else {
                CHECK_EQ(2UL, node.outputs().size());
                EMIT(AveragePool, out(0), out(1), in(0), IntVector(node.kernel_shape()), strides(), pads(), node.count_include_pad());
            }
        } else if (node.op_type() == Node::kSoftmax) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            int axis = node.axis();
            if (axis < 0) axis = 1;
            EMIT(Softmax, out(0), in(0), axis);
        } else if (node.op_type() == Node::kLogSoftmax) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            int axis = node.axis();
            if (axis < 0) axis = 1;
            EMIT(LogSoftmax, out(0), in(0), axis);
        } else if (node.op_type() == Node::kArgMax) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ArgMax, out(0), in(0), node.axis(), node.keepdims());
        } else if (node.op_type() == Node::kHardmax) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Hardmax, out(0), in(0), node.axis());
        } else if (node.op_type() == Node::kReduceMax) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ReduceMax, out(0), in(0), IntVector(node.axes()), node.keepdims());
        } else if (node.op_type() == Node::kReduceSum) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ReduceSum, out(0), in(0), IntVector(node.axes()), node.keepdims());
        } else if (node.op_type() == Node::kReduceSumSquare) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ReduceSumSquare, out(0), in(0), IntVector(node.axes()), node.keepdims());
        } else if (node.op_type() == Node::kOnikuxReduceSumTo) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ReduceSumTo, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kReduceMean) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ReduceMean, out(0), in(0), IntVector(node.axes()), node.keepdims());
        } else if (node.op_type() == Node::kCast) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Cast, out(0), in(0), node.to());
        } else if (node.op_type() == Node::kOneHot) {
            EMIT(OneHot, out(0), in(0), in(1), in(2), node.axis());
        } else if (node.op_type() == Node::kConstantFill) {
            if (node.input_as_shape()) {
                CHECK_EQ(1UL, node.inputs().size());
            } else {
                CHECK_EQ(0UL, node.inputs().size());
            }
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(ConstantFill, out(0), oin(0), node.dtype(), IntVector(node.extra_shape()), IntVector(node.shape()), node.value());
        } else if (node.op_type() == Node::kSlice) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            CHECK_NE(0UL, node.starts().size());
            CHECK_NE(0UL, node.ends().size());
            CHECK_EQ(node.starts().size(), node.ends().size());
            std::vector<int> axes(IntVector(node.axes()));
            if (axes.empty()) {
                for (size_t i = 0; i < node.starts().size(); ++i) axes.push_back(i);
            } else {
                CHECK_EQ(node.starts().size(), axes.size());
            }
            EMIT(Slice, out(0), in(0), axes, IntVector(node.starts()), IntVector(node.ends()));
        } else if (node.op_type() == Node::kDynamicSlice) {
            EMIT(DynamicSlice, out(0), in(0), in(1), in(2), oin(3));
        } else if (node.op_type() == Node::kGather) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Gather, out(0), in(0), in(1), node.axis());
        } else if (node.op_type() == Node::kConcat) {
            CHECK_EQ(1UL, node.outputs().size());
            std::vector<int> ins;
            for (size_t i = 0; i < node.inputs().size(); ++i) ins.push_back(in(i));
            EMIT(Concat, out(0), ins, node.axis());
        } else if (node.op_type() == Node::kSplit) {
            CHECK_EQ(1UL, node.inputs().size());
            std::vector<int> outs;
            for (size_t i = 0; i < node.outputs().size(); ++i) outs.push_back(out(i));
            EMIT(Split, outs, in(0), node.axis(), IntVector(node.split()));
        } else if (node.op_type() == Node::kClip) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Clip, out(0), in(0), node.max(), node.min());
        } else if (node.op_type() == Node::kMax) {
            CHECK_EQ(1UL, node.outputs().size());
            std::vector<int> ins;
            for (size_t i = 0; i < node.inputs().size(); ++i) ins.push_back(in(i));
            EMIT(Max, out(0), ins);
        } else if (node.op_type() == Node::kTranspose) {
            CHECK_EQ(1UL, node.inputs().size());
            CHECK_EQ(1UL, node.outputs().size());
            EMIT(Transpose, out(0), in(0), IntVector(node.perm()));
        } else if (node.op_type() == Node::kOnikuxBatchNormalizationGrad) {
            CHECK_EQ(2UL, node.inputs().size());
            CHECK_EQ(3UL, node.outputs().size());
            EMIT(BatchNormalizationGrad, out(0), out(1), out(2), in(0), in(1));
        } else if (node.op_type() == Node::kOnikuxSelectItemGrad) {
            EMIT(SelectItemGrad, out(0), in(0), in(1), in(2));
        } else if (node.op_type() == Node::kOnikuxGatherGrad) {
            EMIT(GatherGrad, out(0), in(0), in(1), in(2), node.axis());
        } else if (node.op_type() == Node::kOnikuxDynamicSliceGrad) {
            EMIT(DynamicSliceGrad, out(0), in(0), in(1), in(2), in(3), oin(4));
        } else if (node.op_type() == Node::kOnikuxFusionGroup) {
            EmitFusionGroup(node, prog);
        } else if (node.op_type() == Node::kIf) {
            EmitIf(node, prog);
        } else if (node.op_type() == Node::kLoop) {
            EmitLoop(node, prog);
        } else if (node.op_type() == Node::kConstant) {
            EmitConstant(node, prog);
        } else if (node.op_type() == Node::kOnikuxSequenceConstants) {
            EmitConstantSequence(node, prog);
        } else if (node.op_type() == Node::kOnikuxPrint) {
            std::vector<int> ins;
            for (size_t i = 0; i < node.inputs().size(); ++i) ins.push_back(in(i));
            EMIT(Print, ins);
        } else if (node.op_type() == Node::kOnikuxSequenceCreate) {
            EMIT(SequenceCreate, out(0));
        } else if (node.op_type() == Node::kOnikuxSequenceSize) {
            EMIT(SequenceSize, out(0), in(0));
        } else if (node.op_type() == Node::kOnikuxSequenceLengths) {
            EMIT(SequenceLengths, out(0), in(0));
        } else if (node.op_type() == Node::kOnikuxSequenceAppend) {
            if (node.inputs()[0]->users().size() == 1) {
                // Avoid O(N^2) copies for the simple case.
                EMIT(SequenceMove, out(0), in(0));
                EMIT(SequenceAppend, out(0), in(1));
            } else {
                EMIT(SequenceCopy, out(0), in(0));
                EMIT(SequenceAppend, out(0), in(1));
            }
        } else if (node.op_type() == Node::kOnikuxSequencePop) {
            if (node.inputs()[0]->users().size() == 1) {
                // Avoid O(N^2) copies for the simple case.
                EMIT(SequenceMove, out(0), in(0));
                EMIT(SequencePop, out(1), out(0));
            } else {
                EMIT(SequenceCopy, out(0), in(0));
                EMIT(SequencePop, out(1), out(0));
            }
        } else if (node.op_type() == Node::kOnikuxSequenceLookup) {
            EMIT(SequenceLookup, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kOnikuxSequenceGetSlice) {
            EMIT(SequenceGetSlice, out(0), in(0), oin(1), oin(2), oin(3));
        } else if (node.op_type() == Node::kOnikuxSequenceLookupGrad) {
            EMIT(SequenceLookupGrad, out(0), in(0), in(1), in(2));
        } else if (node.op_type() == Node::kOnikuxSequenceGetSliceGrad) {
            EMIT(SequenceGetSliceGrad, out(0), in(0), in(1), oin(2), oin(3), oin(4));
        } else if (node.op_type() == Node::kOnikuxSequenceStack) {
            EMIT(SequenceStack, out(0), in(0), node.axis());
        } else if (node.op_type() == Node::kOnikuxSequenceConcat) {
            if (node.outputs().size() == 1) {
                int tmp_id = next_value_id_++;
                EMIT(SequenceConcat, out(0), tmp_id, in(0), node.axis());
                FREE(tmp_id);
            } else {
                EMIT(SequenceConcat, out(0), out(1), in(0), node.axis());
            }
        } else if (node.op_type() == Node::kOnikuxSequenceSplitAxis) {
            EMIT(SequenceSplitAxis, out(0), in(0), in(1), node.axis());
        } else if (node.op_type() == Node::kOnikuxSequenceSeparate) {
            EMIT(SequenceSeparate, out(0), in(0), node.axis());
        } else if (node.op_type() == Node::kOnikuxSequenceUnpad) {
            EMIT(SequenceUnpad, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kOnikuxSequencePad) {
            EMIT(SequencePad, out(0), in(0), node.length(), node.value());
        } else if (node.op_type() == Node::kOnikuxSequenceRange) {
            EMIT(SequenceRange, out(0), in(0), oin(1), oin(2));
        } else if (node.op_type() == Node::kOnikuxGenericLen) {
            EMIT(GenericLen, out(0), in(0));
        } else if (node.op_type() == Node::kOnikuxGenericGetItem) {
            EMIT(GenericGetItem, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kOnikuxGenericGetSlice) {
            EMIT(GenericGetSlice, out(0), in(0), oin(1), oin(2), oin(3));
        } else if (node.op_type() == Node::kOnikuxGenericAdd) {
            EMIT(GenericAdd, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kOnikuxGenericAccumulateGrad) {
            EMIT(GenericAccumulateGrad, out(0), in(0), in(1));
        } else if (node.op_type() == Node::kOnikuxNullConstant) {
            EMIT(NullConstant, out(0));
        } else {
            CHECK(false) << "Unsupported op: " << node.op_type();
        }
    }

    void EmitConstantImpl(const Node& node, const Tensor* value, int out, bool host, XCProgramProto* prog) {
        Dtype dtype = value->dtype();
        std::vector<int> shape;
        for (int64_t d : value->dims()) {
            CHECK_LE(0, d);
            CHECK_GT(1ULL << 32ULL, d);
            shape.push_back(d);
        }
        if (dtype.IsFloat()) {
            std::vector<double> v;
            for (int64_t i = 0; i < value->NumElements(); ++i) {
                if (dtype.SizeOf() == 4) {
                    v.push_back(value->Get<float>(i));
                } else if (dtype.SizeOf() == 8) {
                    v.push_back(value->Get<double>(i));
                } else {
                    CHECK(false) << "Unknown type: " << dtype;
                }
            }
            if (shape.empty()) {
                EMIT(FloatScalarConstant, out, v[0], dtype, host);
            } else {
                EMIT(FloatConstant, out, v, dtype, shape, host);
            }
        } else {
            std::vector<int64_t> v;
            for (int64_t i = 0; i < value->NumElements(); ++i) {
                if (dtype.SizeOf() == 1) {
                    v.push_back(value->Get<int8_t>(i));
                } else if (dtype.SizeOf() == 2) {
                    v.push_back(value->Get<int16_t>(i));
                } else if (dtype.SizeOf() == 4) {
                    v.push_back(value->Get<int32_t>(i));
                } else if (dtype.SizeOf() == 8) {
                    v.push_back(value->Get<int64_t>(i));
                } else {
                    CHECK(false) << "Unknown type: " << dtype;
                }
            }
            if (shape.empty()) {
                EMIT(IntScalarConstant, out, v[0], dtype, true);
            } else {
                EMIT(IntConstant, out, v, dtype, shape, dtype == Dtype::kInt64);
            }
        }
    }

    void EmitConstant(const Node& node, XCProgramProto* prog) {
        CHECK_EQ(1, node.outputs().size());
        int out = GetValueId(node.outputs()[0]);
        Tensor* value = node.tensor_value().get();
        EmitConstantImpl(node, value, out, node.onikux_host(), prog);
    }

    void EmitConstantSequence(const Node& node, XCProgramProto* prog) {
        CHECK_EQ(1, node.outputs().size());
        std::vector<int> const_values;
        for (const auto& tensor : node.tensor_values()) {
            int id = next_value_id_++;
            EmitConstantImpl(node, tensor.get(), id, false, prog);
            const_values.push_back(id);
        }

        int out = GetValueId(node.outputs()[0]);
        EMIT(SequenceCreate, out);
        for (int id : const_values) {
            EMIT(SequenceAppend, out, id);
            FREE(id);
        }
    }

#undef EMIT

    void EmitGraph(const Graph& graph, XCProgramProto* prog, bool in_loop, const std::vector<Value*>& output_values) {
        std::map<const Value*, int> num_users;
        if (!in_loop) {
            for (const Value* value : graph.input_values()) {
                num_users.emplace(value, value->users().size());
            }
        }
        for (const Value* value : graph.temp_values()) {
            num_users.emplace(value, value->users().size());
        }

        std::set<const Value*> staged_inputs;
        std::set<const Value*> todo_outputs(output_values.begin(), output_values.end());

        std::vector<const Node*> nodes(graph.GetComputationSequence());
        for (const Node* node : nodes) {
            if (!emitted_.emplace(node).second) continue;

            if (!in_loop) {
                for (const Value* value : node->inputs()) {
                    if (!value->IsInput()) continue;
                    if (!staged_inputs.emplace(value).second) continue;
                    AddInOp(prog, GetValueId(value), value->name());
                    prog->mutable_instructions(prog->instructions_size() - 1)->set_debug_info(value->name());
                }
            }

            EmitNode(&graph, *node, prog);

            for (const Value* output : node->outputs()) {
                // Do not free output values.
                if (todo_outputs.erase(output)) continue;
                if (output->IsTemp() && !output->IsNull() && output->users().empty() &&
                    // TODO(hamaji): Figure out how we should handle batch norm.
                    node->op_type() != Node::kBatchNormalization)
                    FREE(GetValueId(output));
            }

            for (const Value* input : node->inputs()) {
                auto found = num_users.find(input);
                if (found == num_users.end()) continue;
                if (--found->second == 0) {
                    FREE(GetValueId(input));
                }
            }
        }
    }

    std::string GetFusionGroupSummary(const Node& node) {
        std::string ret = node.ToString();
        ret += " (";
        ret += JoinString(MapToString(node.subgraph()->nodes(), [](const Node* n) { return Node::OpTypeToString(n->op_type()); }), "+");
        ret += ")";
        return ret;
    }

    void EmitFusionGroup(const Node& node, XCProgramProto* prog) {
        const Graph& body = *node.subgraph();
        CHECK_EQ(node.inputs().size(), body.input_values().size());
        CHECK_EQ(node.outputs().size(), body.output_values().size());
        const std::string& debug_info = node.ToString();

#define EMIT(op, ...)                                               \
    do {                                                            \
        Add##op##Op(prog, __VA_ARGS__);                             \
        FillOpInfo(node, StrCat(debug_info, " @", __LINE__), prog); \
    } while (0)

        if (g_use_tvm && node.fusion_type() == "tvm") {
            std::string dso_filename;
            std::string func_name;
            BuildTVMProgram(body.nodes(), node.onikux_fusion_group(), body.input_values(), body.output_values(), &dso_filename, &func_name);
            if (g_compiler_log) {
                // TODO(hamaji): Show more code.
                CLOG() << "Fusion group (TVM) " << GetFusionGroupSummary(node) << " => " << dso_filename << std::endl;
            }

            std::vector<int> inputs, outputs;
            for (Value* value : node.inputs()) {
                inputs.push_back(GetValueId(value));
            }
            for (Value* value : node.outputs()) {
                outputs.push_back(GetValueId(value));
            }
            // TODO(hamaji): Handle multiple outputs.
            CHECK_EQ(1, node.outputs().size());
            std::vector<int> shape;
            for (int64_t dim : node.outputs()[0]->type().dims()) {
                shape.push_back(dim);
            }
            EMIT(TVM, outputs, inputs, outputs.size(), dso_filename, func_name, shape);
            return;
        }

        if (g_use_nvrtc && node.fusion_type() == "nvrtc") {
            std::string nvrtc;
            BuildNvrtcProgram(body.nodes(), node.onikux_fusion_group(), body.input_values(), body.output_values(), &nvrtc);
            if (g_compiler_log) {
                CLOG() << "Fusion group (NVRTC) " << GetFusionGroupSummary(node) << std::endl;
                CLOG() << nvrtc;
            }

            std::vector<int> inputs, outputs;
            for (Value* value : node.inputs()) {
                inputs.push_back(GetValueId(value));
            }
            for (Value* value : node.outputs()) {
                outputs.push_back(GetValueId(value));
            }
            EMIT(ElementWiseNvrtc, outputs, inputs, outputs.size(), nvrtc, node.onikux_fusion_group());
            return;
        }

        AssignValueIds(body);

        for (size_t i = 0; i < node.inputs().size(); ++i) {
            Value* from = node.inputs()[i];
            Value* to = body.input_values()[i];
            // MOVE(GetValueId(to), GetValueId(from));
            EMIT(Identity, GetValueId(to), GetValueId(from));
        }

        EmitGraph(body, prog, true /* in_loop */, body.output_values());

        // TODO(hamaji): Fix `EmitGraph` so it frees inputs automatically.
        for (size_t i = 0; i < node.inputs().size(); ++i) {
            FREE(GetValueId(body.input_values()[i]));
        }
        for (size_t i = 0; i < node.outputs().size(); ++i) {
            Value* from = body.output_values()[i];
            Value* to = node.outputs()[i];
            if (from->IsNull()) {
                // TODO(hamaji): Consider removing this value.
                EMIT(NullConstant, GetValueId(to));
            } else {
                MOVE(GetValueId(to), GetValueId(from));
            }
        }

#undef EMIT
    }

    void EmitIfImpl(
            const Node& cond,
            Graph* then_body,
            const std::vector<Value*>& then_input_values,
            const std::vector<Value*>& then_output_values,
            Graph* else_body,
            const std::vector<Value*>& else_input_values,
            const std::vector<Value*>& else_output_values,
            XCProgramProto* prog) {
        const std::string& debug_info = cond.ToString();

#define EMIT(op, ...)                                               \
    do {                                                            \
        Add##op##Op(prog, __VA_ARGS__);                             \
        FillOpInfo(cond, StrCat(debug_info, " @", __LINE__), prog); \
    } while (0)

        CHECK_EQ(cond.inputs().size(), then_input_values.size() + 1);
        CHECK_EQ(cond.inputs().size(), else_input_values.size() + 1);
        CHECK_EQ(cond.outputs().size(), then_output_values.size());
        CHECK_EQ(cond.outputs().size(), else_output_values.size());

        auto emit_branch = [this, &cond, prog, &debug_info](
                                   Graph* graph, const std::vector<Value*>& inputs, const std::vector<Value*>& outputs) {
            for (size_t i = 0; i < inputs.size(); ++i) {
                Value* from = cond.inputs()[i + 1];
                Value* to = inputs[i];
                EMIT(Identity, GetValueId(to), GetValueId(from));
            }
            EmitGraph(*graph, prog, true /* in_loop */, outputs);
            // TODO(hamaji): Fix `EmitGraph` so it frees inputs automatically.
            for (size_t i = 0; i < inputs.size(); ++i) {
                FREE(GetValueId(inputs[i]));
            }
            for (size_t i = 0; i < cond.outputs().size(); ++i) {
                Value* from = outputs[i];
                Value* to = cond.outputs()[i];
                if (from->IsNull()) {
                    // TODO(hamaji): Consider removing this value.
                    EMIT(NullConstant, GetValueId(to));
                } else {
                    MOVE(GetValueId(to), GetValueId(from));
                }
            }
        };

        int branch_jmp = prog->instructions_size();
        EMIT(JmpTrue, GetValueId(cond.inputs()[0]), -1);

        emit_branch(else_body, else_input_values, else_output_values);

        int done_jmp = prog->instructions_size();
        EMIT(Jmp, -1);

        runtime::XCInstructionProto* branch = prog->mutable_instructions(branch_jmp);
        branch->mutable_inputs(1)->set_i(prog->instructions_size());

        emit_branch(then_body, then_input_values, then_output_values);

        runtime::XCInstructionProto* done = prog->mutable_instructions(done_jmp);
        done->mutable_inputs(0)->set_i(prog->instructions_size());

#undef EMIT
    }

    void EmitIf(const Node& cond, XCProgramProto* prog) {
        AssignValueIds(*cond.then_branch());
        AssignValueIds(*cond.else_branch());
        EmitIfImpl(
                cond,
                cond.then_branch().get(),
                cond.then_branch()->input_values(),
                cond.then_branch()->output_values(),
                cond.else_branch().get(),
                cond.else_branch()->input_values(),
                cond.else_branch()->output_values(),
                prog);
    }

    void EmitLoopImpl(
            const Node& loop,
            Graph* body,
            const std::vector<Value*>& body_input_values,
            const std::vector<Value*>& body_output_values,
            XCProgramProto* prog) {
        int num_loop_inputs = loop.inputs().size();
        int num_loop_outputs = loop.outputs().size();
        int num_body_inputs = body_input_values.size();
        int num_body_outputs = body_output_values.size();
        int num_states = num_loop_inputs - 2;
        int num_scans = num_body_outputs - 1 - num_states;
        CHECK_EQ(num_body_inputs, num_states + 2) << body->name();
        CHECK_EQ(num_loop_outputs, num_states + num_scans) << body->name();
        Value* max_trip_count = loop.inputs()[0];
        Value* terminal_condition = loop.inputs()[1];
        CHECK(!max_trip_count->IsNull() || !terminal_condition->IsNull()) << "Inifinite loop is detected";

        const std::string& debug_info = loop.ToString();

#define EMIT(op, ...)                                                                                                  \
    do {                                                                                                               \
        Add##op##Op(prog, __VA_ARGS__);                                                                                \
        prog->mutable_instructions(prog->instructions_size() - 1)->set_debug_info(StrCat(debug_info, " @", __LINE__)); \
    } while (0)

        // Initialize loop variables.
        int iter_id = GetValueId(body_input_values[0]);
        EMIT(IntScalarConstant, iter_id, 0, Dtype::kInt64, true);
        int cond_id = GetValueId(body_input_values[1]);
        EMIT(IntScalarConstant, cond_id, 1, Dtype::kBool, true);
        for (int i = 0; i < num_states; ++i) {
            CHECK_LT(i + 2, loop.inputs().size());
            CHECK_LT(i + 2, body_input_values.size());
            const Value* loop_in = loop.inputs()[i + 2];
            const Value* body_in = body_input_values[i + 2];
            EMIT(Identity, GetValueId(body_in), GetValueId(loop_in));
        }

        // Prepare temporary sequences for scan outputs.
        std::vector<int> scan_out_ids;
        for (int i = 0; i < num_scans; ++i) {
            int id = next_value_id_++;
            EMIT(SequenceCreate, id);
            scan_out_ids.push_back(id);
        }

        int skip_loop_jmp = -1;
        int skip_loop_cond_id = -1;
        if (!max_trip_count->IsNull()) {
            int zero_id = next_value_id_++;
            skip_loop_cond_id = next_value_id_++;
            EMIT(IntScalarConstant, zero_id, 0, Dtype::kInt64, true);
            EMIT(Greater, skip_loop_cond_id, GetValueId(max_trip_count), zero_id);
            FREE(zero_id);
        }
        if (!terminal_condition->IsNull()) {
            int tmp_id = next_value_id_++;
            if (skip_loop_cond_id >= 0) {
                EMIT(Mul, tmp_id, skip_loop_cond_id, GetValueId(terminal_condition));
                FREE(skip_loop_cond_id);
            } else {
                EMIT(Identity, tmp_id, GetValueId(terminal_condition));
            }
            skip_loop_cond_id = tmp_id;
        }
        if (skip_loop_cond_id >= 0) {
            skip_loop_jmp = prog->instructions_size();
            EMIT(JmpFalse, skip_loop_cond_id, -1);
        }

        int loop_begin = prog->instructions_size();

        EmitGraph(*body, prog, true /* in_loop */, body_output_values);
        int one_id = next_value_id_++;
        EMIT(IntScalarConstant, one_id, 1, Dtype::kInt64, true);
        int tmp_id = next_value_id_++;
        EMIT(Add, tmp_id, iter_id, one_id);
        FREE(one_id);
        for (const Value* value : body_input_values) {
            FREE(GetValueId(value));
        }
        MOVE(iter_id, tmp_id);
        MOVE(cond_id, GetValueId(body_output_values[0]));

        // Propagate the loop state.
        for (int i = 0; i < num_states; ++i) {
            CHECK_LT(i + 2, body_input_values.size());
            CHECK_LT(i + 1, body_output_values.size());
            const Value* body_in = body_input_values[i + 2];
            const Value* body_out = body_output_values[i + 1];
            if (body_out->IsNull()) {
                // TODO(hamaji): Consider removing this value.
                EMIT(NullConstant, GetValueId(body_in));
            } else {
                MOVE(GetValueId(body_in), GetValueId(body_out));
            }
        }

        // Push scan outputs.
        for (int i = 0; i < num_scans; ++i) {
            CHECK_LT(i + num_states + 1, body_output_values.size());
            const Value* body_out = body_output_values[i + num_states + 1];
            EMIT(SequenceAppend, scan_out_ids[i], GetValueId(body_out));
            FREE(GetValueId(body_out));
        }

        // Check if the loop finishes.
        if (terminal_condition->IsNull()) {
            CHECK(!max_trip_count->IsNull());
            FREE(cond_id);
            EMIT(Greater, cond_id, GetValueId(loop.inputs()[0]), iter_id);
        } else if (!max_trip_count->IsNull()) {
            EMIT(Greater, tmp_id, GetValueId(loop.inputs()[0]), iter_id);
            int tmp2_id = next_value_id_++;
            EMIT(Mul, tmp2_id, cond_id, tmp_id);
            FREE(cond_id);
            MOVE(cond_id, tmp2_id);
            FREE(tmp_id);
        }
        EMIT(JmpTrue, cond_id, loop_begin);

        if (skip_loop_jmp >= 0) {
            runtime::XCInstructionProto* jmp = prog->mutable_instructions(skip_loop_jmp);
            jmp->mutable_inputs(1)->set_i(prog->instructions_size());
            FREE(skip_loop_cond_id);
        }

        // Output final states.
        for (size_t i = 0; i < num_states; ++i) {
            CHECK_LT(i + 2, body_input_values.size());
            CHECK_LT(i, loop.outputs().size());
            const Value* body_in = body_input_values[i + 2];
            const Value* loop_out = loop.outputs()[i];
            if (loop_out->IsNull()) {
                FREE(GetValueId(body_in));
            } else {
                MOVE(GetValueId(loop_out), GetValueId(body_in));
            }
        }

        // Stack and output scan outputs.
        for (int i = 0; i < num_scans; ++i) {
            CHECK_LT(i + num_states, loop.outputs().size());
            const Value* loop_out = loop.outputs()[i + num_states];
            EMIT(SequenceStack, GetValueId(loop_out), scan_out_ids[i], loop.onikux_stack_axis());
            FREE(scan_out_ids[i]);
        }

        FREE(iter_id);
        FREE(cond_id);

#undef EMIT
    }

    void EmitLoop(const Node& loop, XCProgramProto* prog) {
        AssignValueIds(*loop.body());
        EmitLoopImpl(loop, loop.body().get(), loop.body()->input_values(), loop.body()->output_values(), prog);
    }

    void EmitOutputs(const std::vector<Value*>& output_values, XCProgramProto* prog) {
        for (const Value* value : output_values) {
            AddOutOp(prog, value->name(), GetValueId(value));
            prog->mutable_instructions(prog->instructions_size() - 1)->set_debug_info(value->name());
            FREE(GetValueId(value));
        }
    }

    int next_value_id_{1};
    std::map<const Value*, int> value_ids_;
    std::map<int, int> stack_ids_;
    std::set<const Node*> emitted_;
};

}  // namespace

void Emit(const Model& model, XCProgramProto* program, bool dump_value_names) {
    Emit(model.graph(), program, dump_value_names);
}

void Emit(const Graph& graph, XCProgramProto* program, bool dump_value_names) {
    XCVMEmitter emitter;
    emitter.EmitModel(graph, program, dump_value_names);
}

void Emit(const Model& model, std::ostream& out, bool dump_value_names) {
    XCProgramProto program;
    Emit(model, &program, dump_value_names);
    CHECK(program.SerializeToOstream(&out));
}

void Emit(
        const std::vector<Node*>& nodes,
        const std::vector<Value*>& fetches,
        runtime::XCProgramProto* program,
        std::vector<int>* output_ids) {
    XCVMEmitter emitter;
    std::set<Value*> values;
    for (Node* node : nodes) {
        for (Value* value : node->inputs()) values.insert(value);
        for (Value* value : node->outputs()) values.insert(value);
    }
    emitter.AssignValueIds(values);
    for (Value* v : fetches) output_ids->push_back(emitter.GetValueId(v));
    emitter.EmitNodes(nodes, program);
}

}  // namespace xcvm
}  // namespace oniku
