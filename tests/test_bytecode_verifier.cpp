#include "bytecode.h"
#include "verifier.h"

#include <cstdio>
#include <memory>
#include <string>

using skein::bc::Constant;
using skein::bc::ConstantKind;
using skein::bc::Instr;
using skein::bc::Op;
using skein::bc::Proto;

static Instr make(Op op, uint8_t flags = 0, uint16_t a = 0,
                  uint32_t b = 0) {
    Instr ins;
    ins.op = static_cast<uint8_t>(op);
    ins.flags = flags;
    ins.a = a;
    ins.b = b;
    return ins;
}

static int failures = 0;

static void expect(bool condition, const char *name) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++failures;
    }
}

int main() {
    {
        Proto proto;
        proto.consts.push_back(Constant{});
        proto.code.push_back(make(Op::LoadConst, 0, 0, 0));
        proto.code.push_back(make(Op::Return));
        std::string error;
        uint32_t max_stack = 0;
        expect(skein::bc::verify(proto, error, &max_stack),
               "valid proto verifies");
        expect(max_stack == 1, "valid proto computes max stack");
    }
    {
        Proto proto;
        proto.code.push_back(make(Op::Jump, 0, 0, 99));
        std::string error;
        expect(!skein::bc::verify(proto, error), "jump target rejected");
    }
    {
        Proto proto;
        proto.code.push_back(make(Op::Add));
        std::string error;
        expect(!skein::bc::verify(proto, error), "stack underflow rejected");
    }
    {
        Proto proto;
        proto.code.push_back(make(Op::LoadNull));
        proto.code.push_back(make(Op::JumpIfFalse, 0, 0, 4));
        proto.code.push_back(make(Op::LoadNull));
        proto.code.push_back(make(Op::Jump, 0, 0, 5));
        proto.code.push_back(make(Op::Nop));
        proto.code.push_back(make(Op::Pop));
        std::string error;
        expect(!skein::bc::verify(proto, error),
               "inconsistent stack depth rejected");
    }
    {
        Proto proto;
        proto.code.push_back(make(Op::PopHandler));
        std::string error;
        expect(!skein::bc::verify(proto, error), "handler underflow rejected");
    }
    {
        Proto proto;
        proto.code.push_back(make(Op::PushFailHandler, 0, 0, 2));
        proto.code.push_back(make(Op::PopHandler));
        proto.code.push_back(make(Op::LoadNull));
        proto.code.push_back(make(Op::Return));
        std::string error;
        expect(skein::bc::verify(proto, error), "balanced handler accepted");
    }
    {
        Proto child;
        child.code.push_back(make(Op::LoadNull));
        child.code.push_back(make(Op::Return));
        std::string error;
        expect(skein::bc::verify(child, error), "child proto verifies");

        auto body = std::make_shared<const Proto>(std::move(child));
        Proto parent;
        skein::bc::PackageDesc package;
        package.name = "p";
        package.body = body;
        parent.packages.push_back(std::move(package));
        parent.code.push_back(make(Op::DeclarePackage, 0, 0, 0));
        parent.code.push_back(make(Op::Return));
        expect(skein::bc::verify_tree(parent, error),
               "proto tree verifies");
    }
    {
        Proto proto;
        proto.code.push_back(make(Op::LoadConst, 0, 0, 0));
        proto.code.push_back(make(Op::Return));
        std::string error;
        expect(!skein::bc::verify(proto, error),
               "missing constant rejected");
    }
    {
        Proto proto;
        proto.code.push_back(make(Op::DefineName, 0, 0, 0));
        proto.code.push_back(make(Op::Return));
        std::string error;
        expect(!skein::bc::verify(proto, error),
               "missing declaration rejected");
    }

    if (failures != 0)
        return 1;
    std::printf("bytecode verifier tests passed\n");
    return 0;
}
