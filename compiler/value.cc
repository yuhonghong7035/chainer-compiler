#include "compiler/value.h"

#include <algorithm>

#include <common/log.h>
#include <common/strutil.h>
#include <compiler/serializer_util.h>
#include <compiler/tensor.h>
#include <compiler/type.h>

namespace chainer_compiler {

Value::Value(const onnx::ValueInfoProto& xvalue, Kind kind) : Value(xvalue.name(), Type(xvalue.type()), kind) {
    doc_string_ = xvalue.doc_string();
}

Value::Value(const std::string& name, Kind kind) : Value(name, Type(), kind) {
}

Value::Value(const std::string& name, const Type& type, Kind kind) : kind_(kind), name_(name), type_(new Type(type)) {
    if (name_ == "") kind_ = static_cast<Value::Kind>(kind_ | Value::Kind::kNull);
}

Value::~Value() {
    CHECK(grad_ == nullptr);
}

void Value::ToONNX(onnx::ValueInfoProto* xvalue) const {
    DUMP_STRING(xvalue, name);
    type_->ToONNX(xvalue->mutable_type());
    DUMP_STRING(xvalue, doc_string);
}

std::string Value::DebugString() const {
    onnx::ValueInfoProto xvalue;
    ToONNX(&xvalue);
    return xvalue.DebugString();
}

void Value::ResetInitializer(std::unique_ptr<Tensor>&& tensor) {
    initializer_.reset(tensor.release());
}

void Value::set_type(Type* type) {
    type_.reset(type);
}

int64_t Value::GetNBytes() const {
    return type_->GetNBytes();
}

void Value::AddUser(Node* user) {
    users_.push_back(user);
}

void Value::DetachUser(const Node* user) {
    auto found = std::find(users_.begin(), users_.end(), user);
    CHECK(found != users_.end());
    users_.erase(found);
}

void Value::SetProducer(Node* producer) {
    producer_ = producer;
}

void Value::set_grad(Value* grad) {
    grad_ = grad;
    if (grad_ && (type_->kind() != Type::Kind::kTensor || type_->HasKnownShape())) {
        grad_->set_type(new Type(*type_));
    }
}

std::ostream& operator<<(std::ostream& os, const Value::Kind& kind) {
    if (kind == Value::Kind::kTemp) {
        return os << "Temp";
    }
    std::vector<std::string> out;
    if (kind & Value::Kind::kInput) out.push_back("Input");
    if (kind & Value::Kind::kOutput) out.push_back("Output");
    if (kind & Value::Kind::kNull) out.push_back("Null");
    if (out.empty()) {
        os << "???(" << kind << ")";
    } else {
        os << JoinString(out, "|");
    }
    return os;
}

}  // namespace chainer_compiler
