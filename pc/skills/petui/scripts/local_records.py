"""Compile creator record actions into ordinary transactional P4 integer-program data."""
import argparse
from copy import deepcopy
import json
from pathlib import Path

from component_program import validate


def integer(value, lo, hi):
    return type(value) is int and lo <= value <= hi


class RecordImage:
    """One presence cell plus fixed-width signed words per record. Cell zero is the app schema."""

    def __init__(self, spec):
        self.width = spec["words"]
        self.user = spec["user_slots"]
        self.cache = spec["cache_slots"]
        self.version = spec["version"]
        if not (integer(self.width, 1, 6) and integer(self.user, 1, 127)
                and integer(self.cache, 0, 127) and integer(self.version, 1, 65535)):
            raise ValueError("invalid record quotas or version")
        self.stride = self.width + 1
        self.cells = 1 + self.stride * (self.user + self.cache)
        if self.cells > 256:
            raise ValueError("record layout exceeds 256 cells")
        self.code = []
        self.labels = {}
        self.loop_fuel = 0

    def emit(self, op, a=0, b=0, c=0, imm=0):
        self.code.append([op, a, b, c, imm])

    def label(self, name):
        self.labels[name] = len(self.code)

    def const(self, reg, value):
        self.emit("CONST", reg, imm=value)

    def jump(self, name):
        self.emit("JUMP", imm=name)

    def value(self, reg, source, inputs):
        if integer(source, -2**31, 2**31-1):
            self.const(reg, source)
        elif isinstance(source, str) and source in inputs:
            self.emit("EVENT", reg, imm=1 + inputs.index(source))
        else:
            raise ValueError("expected int32 constant or declared action input")

    def address(self, space):
        # r3 is a checked, zero-based key. r0 receives the presence cell address.
        self.const(0, 1 + (self.user * self.stride if space == "cache" else 0))
        self.emit("MUL", 2, 3, 8)
        self.emit("ADD", 0, 0, 2)

    def action(self, name, action, index):
        if set(action) - {"inputs", "batch", "from_version"}:
            raise ValueError("unknown record action field")
        inputs = action.get("inputs", [])
        if (not isinstance(inputs, list) or len(inputs) > 3 or
                not all(isinstance(v, str) for v in inputs) or len(set(inputs)) != len(inputs)):
            raise ValueError("record action needs up to three distinct input variables")
        batch = action["batch"]
        if not isinstance(batch, list) or not 1 <= len(batch) <= 8:
            raise ValueError("record action needs 1..8 operations")
        expected = action.get("from_version")
        if "from_version" in action and not integer(expected, 1, self.version-1):
            raise ValueError("migration must advance from a specific older version")
        self.label(name)
        # A mismatched logical schema leaves all record cells unchanged.
        self.const(0, 0)
        self.emit("LOAD", 5, 0)
        if expected is None:
            self.emit("JZ", 5, imm=(index, "admit"))
        self.const(1, expected or self.version)
        self.emit("EQ", 4, 5, 1)
        self.emit("JZ", 4, imm="wrong version")
        self.label((index, "admit"))

        # Check every dynamic operand before touching any row, even late in a batch.
        loop_fuel = 0
        for n, row in enumerate(batch):
            if not isinstance(row, list) or len(row) < 2:
                raise ValueError("invalid record operation")
            op, space, *args = row
            if space not in ("user", "cache"):
                raise ValueError("unknown record space")
            cap = self.user if space == "user" else self.cache
            if not cap:
                raise ValueError("space has zero quota")
            if op == "map_add":
                if (expected is None or n == len(batch)-1 or len(args) != 2
                        or not integer(args[0], 0, self.width-1)
                        or not integer(args[1], -2**31, 2**31-1)):
                    raise ValueError("map_add needs migration, column, delta and a following operation")
                loop_fuel += 16 * cap
                continue
            if op in ("next", "append"):
                loop_fuel += 8 * cap
            if op not in ("get", "next", "delete", "put", "append"):
                raise ValueError("unknown record operation")
            if op == "append":
                if n or len(args) != self.width:
                    raise ValueError("append must reserve space first and provide a whole row")
                continue
            if len(args) != (1 + self.width if op == "put" else 1):
                raise ValueError("wrong record operand count")
            key = args[0]
            limit = cap + (op == "next")
            if type(key) is int:
                if not integer(key, 0, limit-1):
                    raise ValueError("constant key outside quota")
            else:
                self.value(3, key, inputs)
                self.const(1, 0)
                self.emit("LT", 4, 3, 1)
                self.emit("JZ", 4, imm=(index, n, "nonnegative"))
                self.jump("bad key")
                self.label((index, n, "nonnegative"))
                self.const(1, limit)
                self.emit("LT", 4, 3, 1)
                self.emit("JZ", 4, imm="bad key")
            if op in ("get", "next") and n != len(batch)-1:
                raise ValueError("read operations must finish the batch")

        self.loop_fuel = max(self.loop_fuel, loop_fuel)
        for n, (op, space, *args) in enumerate(batch):
            if op == "map_add":
                # Transform only occupied rows. VM overflow rolls back all
                # changes, including earlier rows and the logical schema.
                col, delta = args
                self.const(3, 0)
                self.address(space)
                self.const(1, self.user if space == "user" else self.cache)
                self.const(11, col+1)
                self.const(12, delta)
                self.label((index, n, "map"))
                self.emit("LOAD", 4, 0)
                self.emit("JZ", 4, imm=(index, n, "skip"))
                self.emit("ADD", 9, 0, 11)
                self.emit("LOAD", 4, 9)
                self.emit("ADD", 4, 4, 12)
                self.emit("STORE", 9, 4)
                self.label((index, n, "skip"))
                self.emit("ADD", 0, 0, 8)
                self.emit("ADD", 3, 3, 7)
                self.emit("EQ", 4, 3, 1)
                self.emit("JZ", 4, imm=(index, n, "map"))
                continue
            if op == "append":
                self.const(3, 0)
            else:
                self.value(3, args.pop(0), inputs)
            self.address(space)
            if op in ("next", "append"):
                cap = self.user if space == "user" else self.cache
                self.const(1, cap)
                self.label((index, n, "scan"))
                self.emit("LT", 4, 3, 1)
                self.emit("JZ", 4, imm="full" if op == "append" else (index, "end scan"))
                self.emit("LOAD", 4, 0)
                if op == "next":
                    self.emit("EQ", 4, 4, 6)  # zero means absent (r6=0)
                self.emit("JZ", 4, imm=(index, n, "found"))
                self.emit("ADD", 3, 3, 7)
                self.emit("ADD", 0, 0, 8)
                self.jump((index, n, "scan"))
                self.label((index, n, "found"))
            if op in ("append", "put", "delete"):
                self.emit("STORE", 0, 6 if op == "delete" else 7)
                self.emit("MOVE", 9, 0)
                for col in range(self.width):
                    self.emit("ADD", 9, 9, 7)
                    if op != "delete":
                        self.value(1, args[col], inputs)
                    self.emit("STORE", 9, 6 if op == "delete" else 1)
        self.emit("MOVE", 9, 0)
        self.const(1, self.version)
        self.emit("STORE", 6, 1)
        self.jump("read row")
        if batch[-1][0] == "next":
            self.label((index, "end scan"))
            # Prior batch writes and a migration are also committed at end of list.
            self.const(1, self.version)
            self.emit("STORE", 6, 1)
            self.jump("missing")
        return [index, *({"var": v} for v in inputs)]

    def program(self, actions, outputs):
        if not isinstance(actions, dict) or not 1 <= len(actions) <= 24:
            raise ValueError("expected 1..24 record actions")
        if not isinstance(outputs, list) or len(outputs) != 2+self.width:
            raise ValueError("outputs must name status, key and each record word")
        self.const(6, 0)
        self.const(7, 1)
        self.const(8, self.stride)
        self.emit("EVENT", 10, imm=0)
        for i, name in enumerate(actions):
            self.const(1, i)
            self.emit("EQ", 4, 10, 1)
            self.emit("JZ", 4, imm=(i, "dispatch"))
            self.jump(("action", name))
            self.label((i, "dispatch"))
        self.jump("bad key")
        words = {name: self.action(("action", name), action, i)
                 for i, (name, action) in enumerate(actions.items())}
        self.label("read row")
        self.emit("LOAD", 4, 9)
        self.emit("JZ", 4, imm="missing")
        self.emit("OUTPUT", 0, 7)
        self.emit("OUTPUT", 1, 3)
        for col in range(self.width):
            self.emit("ADD", 9, 9, 7)
            self.emit("LOAD", 1, 9)
            self.emit("OUTPUT", 2+col, 1)
        self.emit("HALT")
        for label, status in (("missing", 0), ("bad key", -1), ("full", -2), ("wrong version", -3)):
            self.label(label)
            self.const(1, status)
            self.jump("finish")
        self.label("finish")
        self.emit("OUTPUT", 0, 1)
        self.const(1, -1)
        self.emit("OUTPUT", 1, 1)
        for col in range(self.width):
            self.emit("OUTPUT", 2+col, 6)
        self.emit("HALT")
        for row in self.code:
            if row[0] in ("JUMP", "JZ"):
                row[-1] = self.labels[row[-1]]
        wide = self.cells > 64 or len(self.code) > 256
        if len(self.code) > 1024:
            raise ValueError("record actions exceed instruction quota")
        fuel = len(self.code) + self.loop_fuel
        if fuel > 4096:
            raise ValueError("record action exceeds the device fuel quota")
        v = 2 if wide else 1
        schema = 0x52000000 | self.width << 16 | self.user << 8 | self.cache
        return {"engine": f"p4-component-program-v{v}", "actions": words, "timers": [],
                "outputs": outputs + [None] * (6-self.width),
                "image": {"engine": f"p4-integer-program-v{v}", "code": self.code,
                          "cells": self.cells, "schema": schema, "fuel": fuel}}


def compile_runtime(source):
    runtime = deepcopy(source)
    spec = runtime.pop("records")
    if set(spec) != {"version", "words", "user_slots", "cache_slots", "outputs", "actions"}:
        raise ValueError("invalid record definition")
    if "program" in runtime:
        raise ValueError("records require their own program")
    runtime["program"] = RecordImage(spec).program(spec["actions"], spec["outputs"])
    for name in spec["actions"]:
        if not any(rule.get("on") == name for rule in runtime.setdefault("transitions", [])):
            runtime["transitions"].append({"from": "*", "on": name})
    validate(runtime)
    limit = 12288 if runtime.get("quota") == "p4-quota-v3" else 8192
    if len(json.dumps(runtime, ensure_ascii=False, separators=(",", ":")).encode()) >= limit:
        raise ValueError("compiled runtime exceeds the device install quota")
    return runtime


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    print(json.dumps(compile_runtime(json.loads(args.source.read_text(encoding="utf-8"))),
                     ensure_ascii=False, separators=(",", ":")))
