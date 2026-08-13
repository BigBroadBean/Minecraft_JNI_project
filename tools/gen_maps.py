#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_maps.py —— 从 mappings-extracted 自动生成 MCCombatStatusJni 的 JniMap 映射表。

数据来源: D:\\VibeCoding\\mappings-extracted (54 个版本, 6 命名空间)
产物:
  * --report : 打印每个版本的解析结果 (人工核对用)
  * --emit   : 生成 src/mc_maps_generated.h (可直接编译进 DLL 的 C++ 表)

命名空间 (CSV 列固定顺序):
  classes.csv : 混淆的,Searge,Spigot,Mojang,Yarn,Intermediary
  fields.csv  : owner_obf,混淆的,Searge,Spigot,Mojang,Yarn,Intermediary,kind,type[,side,desc]
  methods.csv : owner_obf,混淆的,Searge,Spigot,Mojang,Yarn,Intermediary,kind,signature,descriptor,return_type[,side,desc]

三种运行时形态 (由数据自动判定):
  S1 (1.8.8~1.13.2)  字段式, SRG 名 (func_/field_)
  S2 (1.14~1.16.5)   getter 式, SRG 名 (func_/field_)
  S3 (1.17+)          getter 式, Mojang 类名 + MCP stable 名 (m_/f_)
"""
import csv, os, re, sys, argparse

BASE = r"D:\VibeCoding\mappings-extracted"
OUT_H = r"D:\VibeCoding\MCCombatStatusJni\src\mc_maps_generated.h"

# ---------------------------------------------------------------------------
# 类名规范候选 (按版本演进顺序排列, 取第一个在数据中存在的)
# ---------------------------------------------------------------------------
CLASS_CANDIDATES = {
    "mc": ["net/minecraft/client/Minecraft"],
    "player": {
        "searge": ["net/minecraft/client/entity/EntityPlayerSP",
                   "net/minecraft/client/entity/player/ClientPlayerEntity"],
        "mojang": ["net/minecraft/client/player/LocalPlayer"],
    },
    "mop": {
        "searge": ["net/minecraft/util/MovingObjectPosition",
                   "net/minecraft/util/math/RayTraceResult"],
        "mojang": ["net/minecraft/world/phys/HitResult"],
    },
    "type": {
        "searge": ["net/minecraft/util/MovingObjectPosition$MovingObjectType",
                   "net/minecraft/util/math/RayTraceResult$Type"],
        "mojang": ["net/minecraft/world/phys/HitResult$Type"],
    },
    "entityHitCls": {
        "searge": ["net/minecraft/util/math/EntityRayTraceResult"],
        "mojang": ["net/minecraft/world/phys/EntityHitResult"],
    },
    "entity": {
        "searge": ["net/minecraft/entity/Entity"],
        "mojang": ["net/minecraft/world/entity/Entity"],
    },
    "living": {
        "searge": ["net/minecraft/entity/EntityLivingBase",
                   "net/minecraft/entity/LivingEntity"],
        "mojang": ["net/minecraft/world/entity/LivingEntity"],
    },
    "itemStack": {
        "searge": ["net/minecraft/item/ItemStack"],
        "mojang": ["net/minecraft/world/item/ItemStack"],
    },
    "item": {
        "searge": ["net/minecraft/item/Item"],
        "mojang": ["net/minecraft/world/item/Item"],
    },
    "itemBlock": {
        "searge": ["net/minecraft/item/ItemBlock",
                   "net/minecraft/item/BlockItem"],
        "mojang": ["net/minecraft/world/item/BlockItem"],
    },
}

NS_COLS = {"obf": 0, "searge": 1, "spigot": 2, "mojang": 3, "yarn": 4, "intermediary": 5}


def load_csv(path):
    with open(path, encoding="utf-8-sig", newline="") as f:
        return list(csv.reader(f))


def parse_srg(path):
    """md[(srg_class, srg_name)] -> [dict]; fd[(srg_class, srg_name)] -> dict."""
    md, fd = {}, {}
    for ln in open(path, encoding="utf-8-sig"):
        ln = ln.rstrip("\n")
        if ln.startswith("MD: "):
            p = ln[4:].split()
            if len(p) < 4:
                continue
            obfc, obfn = p[0].rsplit("/", 1)
            srgc, srgn = p[2].rsplit("/", 1)
            md.setdefault((srgc, srgn), []).append(
                dict(obfc=obfc, obfn=obfn, obfd=p[1], srgc=srgc, srgn=srgn, srgd=p[3]))
        elif ln.startswith("FD: "):
            p = ln[4:].split()
            if len(p) < 2:
                continue
            obfc, obfn = p[0].rsplit("/", 1)
            srgc, srgn = p[1].rsplit("/", 1)
            fd[(srgc, srgn)] = dict(obfc=obfc, obfn=obfn, srgc=srgc, srgn=srgn)
    return md, fd


def strip_call(s):
    if s and s.endswith(")") and "(" in s:
        return s[:s.rindex("(")]
    return s


class Version:
    def __init__(self, ver):
        self.ver = ver
        d = os.path.join(BASE, ver)
        self.classes = load_csv(os.path.join(d, "classes.csv"))
        self.fields = load_csv(os.path.join(d, "fields.csv"))
        self.methods = load_csv(os.path.join(d, "methods.csv"))
        self.md, self.fd = parse_srg(os.path.join(d, "joined.srg"))
        self.cls = {ns: {} for ns in NS_COLS}
        for row in self.classes[1:]:
            if len(row) < 6:
                continue
            for ns, i in NS_COLS.items():
                if row[i]:
                    self.cls[ns][row[i]] = row
        self.fld_obf = {}   # (owner_obf, obf_name) -> row
        self.fld_srg = {}   # (owner_obf, searge_name) -> row
        for row in self.fields[1:]:
            if len(row) >= 7:
                self.fld_obf[(row[0], row[1])] = row
                if row[2]:
                    self.fld_srg[(row[0], row[2])] = row
        self.mth_obf = {}   # (owner_obf, obf_name) -> row
        self.mth_srg = {}   # (owner_obf, searge_name) -> row
        self.mth_moj = {}   # (owner_obf, mojang_name) -> row
        for row in self.methods[1:]:
            if len(row) < 11:
                continue
            self.mth_obf[(row[0], strip_call(row[1]))] = row
            if row[2]:
                self.mth_srg[(row[0], strip_call(row[2]))] = row
            if row[4]:
                self.mth_moj[(row[0], strip_call(row[4]))] = row

    def shape(self):
        if any(k[1] == "func_71410_x" for k in self.md):
            return "S1" if any(k[1] == "field_72313_a" for k in self.fd) else "S2"
        return "S3"

    def resolve_class(self, role, key_ns):
        cand = CLASS_CANDIDATES[role]
        if isinstance(cand, dict):
            cand = cand[key_ns]
        for c in cand:
            if c in self.cls[key_ns]:
                return self.cls[key_ns][c]
        return None

    def find_md(self, srg_class, srg_name):
        return self.md.get((srg_class, srg_name))

    def find_fd(self, srg_class, srg_name):
        return self.fd.get((srg_class, srg_name))

    def entity_constant(self, type_obf):
        """在 type 类里找 ENTITY 枚举常量, 返回完整字段行 (或 None)。"""
        for row in self.fields[1:]:
            if len(row) < 7 or row[0] != type_obf:
                continue
            if "ENTITY" in (row[2], row[3], row[4]):
                return row
        return None


def resolve(v):
    shape = v.shape()
    era = "A" if shape in ("S1", "S2") else "B"
    key_ns = "searge" if era == "A" else "mojang"

    C = {role: v.resolve_class(role, key_ns)
         for role in ["mc", "player", "mop", "type", "entityHitCls",
                      "entity", "living", "itemStack", "item", "itemBlock"]}

    if era == "A":
        keys = dict(getMc="func_71410_x", playerFd="field_71439_g", mopFd="field_71476_x",
                    typeFd="field_72313_a", typeGet="func_216346_c",
                    entFd="field_72308_g", entGet="func_216348_a",
                    canAttack="func_70075_an", isAlive="func_70089_S",
                    heldOld="func_70694_bm", held="func_184614_ca", itemGet="func_77973_b")
    else:
        keys = dict(getMc="m_91087_", playerFd="f_91074_", mopFd="f_91077_",
                    typeGet="m_6662_", entGet="m_82443_",
                    isAlive="m_6084_", isAttackable="m_6097_",
                    held="m_21205_", itemGet="m_41720_")

    def md_of(role, key):
        c = C[role]
        if c is None:
            return None
        hits = v.find_md(c[NS_COLS["searge"]], key)
        return hits[0] if hits else None

    def fd_of(role, key):
        c = C[role]
        if c is None:
            return None
        return v.find_fd(c[NS_COLS["searge"]], key)

    R = {"shape": shape, "era": era, "C": C,
         "missing_classes": [r for r, c in C.items() if c is None]}
    R["getMc"] = md_of("mc", keys["getMc"])
    R["playerFd"] = fd_of("mc", keys["playerFd"])
    R["mopFd"] = fd_of("mc", keys["mopFd"])

    if shape == "S1":
        R["typeFd"] = fd_of("mop", keys["typeFd"])
        R["typeGet"] = None
        R["entFd"] = fd_of("mop", keys["entFd"])
        R["entGet"] = None
    else:
        R["typeFd"] = None
        R["typeGet"] = md_of("mop", keys["typeGet"])
        R["entFd"] = None
        R["entGet"] = md_of("entityHitCls", keys["entGet"])

    R["isAlive"] = md_of("entity", keys["isAlive"])
    R["canAttack"] = md_of("entity", keys["canAttack"]) if "canAttack" in keys else None
    R["isAttackable"] = md_of("entity", keys["isAttackable"]) if "isAttackable" in keys else None
    if "heldOld" in keys:
        R["held"] = md_of("living", keys["heldOld"]) or md_of("living", keys["held"])
    else:
        R["held"] = md_of("living", keys["held"])
    R["itemGet"] = md_of("itemStack", keys["itemGet"])

    R["ent_row"] = v.entity_constant(C["type"][NS_COLS["obf"]]) if C["type"] is not None else None
    R["ent_obf"] = R["ent_row"][1] if R["ent_row"] else None
    R["ent_int"] = R["ent_row"][6] if (R["ent_row"] and len(R["ent_row"]) > 6) else None
    R["ent_ok"] = R["ent_row"] is not None
    return R


def cstr(s):
    return "NULL" if s is None else '"%s"' % s


def L(name):
    return "L" + name + ";"


def M(name):
    """静态无参方法描述符, 如 ()Lenn; (getMinecraft 等)。"""
    return "()" + L(name)


def build_maps(v, R):
    ver = v.ver
    C, shape, era = R["C"], R["shape"], R["era"]

    def cl(role, ns):
        c = C[role]
        return c[NS_COLS[ns]] if c else None

    def moj_field(owner_obf, obf_name):
        row = v.fld_obf.get((owner_obf, obf_name))
        return row[4] if row and len(row) > 4 else None

    def moj_method(owner_obf, candidates):
        for c in candidates:
            row = v.mth_moj.get((owner_obf, c))
            if row:
                return strip_call(row[4])
        return None

    def int_member(rec, is_field):
        """按稳定名 (Searge 列, 唯一) 查 Intermediary 名 (Fabric 运行时)。
        不能用 obf 名查: 混淆方法名会重载撞名 (如 getItem 的 obf 名 'h' 不唯一)。"""
        if not rec:
            return None
        row = (v.fld_srg if is_field else v.mth_srg).get((rec["obfc"], rec["srgn"]))
        if not row or len(row) <= 6 or not row[6]:
            return None
        return row[6] if is_field else strip_call(row[6])

    maps = []

    # ---- vanilla (obf 类 + obf 成员) ----
    mc_obf = cl("mc", "obf")
    m = {
        "name": "vanilla" + ver.replace(".", ""),
        "mcClass": mc_obf,
        "mcSig": M(mc_obf) if mc_obf else None,
        "getMinecraft": R["getMc"]["obfn"] if R["getMc"] else None,
        "thePlayerField": R["playerFd"]["obfn"] if R["playerFd"] else None,
        "playerFieldSig": L(cl("player", "obf")) if cl("player", "obf") else None,
        "mopField": R["mopFd"]["obfn"] if R["mopFd"] else None,
        "mopFieldSig": L(cl("mop", "obf")) if cl("mop", "obf") else None,
        "mopClass": cl("mop", "obf"),
        "typeOfHitField": R["typeFd"]["obfn"] if R["typeFd"] else None,
        "typeOfHitGetter": R["typeGet"]["obfn"] if R["typeGet"] else None,
        "typeOfHitSig": None,
        "entityHitClass": cl("entityHitCls", "obf"),
        "entityHitField": R["entFd"]["obfn"] if R["entFd"] else None,
        "entityHitGetter": R["entGet"]["obfn"] if R["entGet"] else None,
        "entityHitSig": None,
        "typeClass": cl("type", "obf"),
        "entityConstField": R["ent_obf"],
        "entityConstAlt": None,
        "entityConstSig": L(cl("type", "obf")) if cl("type", "obf") else None,
        "entityClass": cl("entity", "obf"),
        "canAttackWithItem": R["canAttack"]["obfn"] if R["canAttack"] else None,
        "isAliveMethod": R["isAlive"]["obfn"] if R["isAlive"] else None,
        "isAttackable": R["isAttackable"]["obfn"] if R["isAttackable"] else None,
        "livingClass": cl("living", "obf"),
        "heldItemGetter": R["held"]["obfn"] if R["held"] else None,
        "heldItemSig": M(cl("itemStack", "obf")) if cl("itemStack", "obf") else None,
        "itemStackClass": cl("itemStack", "obf"),
        "itemGetItem": R["itemGet"]["obfn"] if R["itemGet"] else None,
        "itemGetItemSig": M(cl("item", "obf")) if cl("item", "obf") else None,
        "itemBlockClass": cl("itemBlock", "obf"),
    }
    if m["typeOfHitField"]:
        m["typeOfHitSig"] = L(cl("type", "obf"))
    elif m["typeOfHitGetter"]:
        m["typeOfHitSig"] = "()" + L(cl("type", "obf"))
    if m["entityHitField"]:
        m["entityHitSig"] = L(cl("entity", "obf"))
    elif m["entityHitGetter"]:
        m["entityHitSig"] = "()" + L(cl("entity", "obf"))
    maps.append(m)

    # ---- forge (era A: MCP 类 + SRG 成员; era B: Mojang 类 + stable 成员) ----
    cls_ns = "searge" if era == "A" else "mojang"

    def srg_name(rec):
        return rec["srgn"] if rec else None

    m = {
        "name": "forge" + ver.replace(".", ""),
        "mcClass": cl("mc", cls_ns),
        "mcSig": M(cl("mc", cls_ns)) if cl("mc", cls_ns) else None,
        "getMinecraft": srg_name(R["getMc"]),
        "thePlayerField": R["playerFd"]["srgn"] if R["playerFd"] else None,
        "playerFieldSig": L(cl("player", cls_ns)) if cl("player", cls_ns) else None,
        "mopField": R["mopFd"]["srgn"] if R["mopFd"] else None,
        "mopFieldSig": L(cl("mop", cls_ns)) if cl("mop", cls_ns) else None,
        "mopClass": cl("mop", cls_ns),
        "typeOfHitField": R["typeFd"]["srgn"] if R["typeFd"] else None,
        "typeOfHitGetter": srg_name(R["typeGet"]),
        "typeOfHitSig": None,
        "entityHitClass": cl("entityHitCls", cls_ns),
        "entityHitField": R["entFd"]["srgn"] if R["entFd"] else None,
        "entityHitGetter": srg_name(R["entGet"]),
        "entityHitSig": None,
        "typeClass": cl("type", cls_ns),
        "entityConstField": "ENTITY" if R["ent_ok"] else None,
        "entityConstAlt": R["ent_obf"],
        "entityConstSig": L(cl("type", cls_ns)) if cl("type", cls_ns) else None,
        "entityClass": cl("entity", cls_ns),
        "canAttackWithItem": srg_name(R["canAttack"]),
        "isAliveMethod": srg_name(R["isAlive"]),
        "isAttackable": srg_name(R["isAttackable"]),
        "livingClass": cl("living", cls_ns),
        "heldItemGetter": srg_name(R["held"]),
        "heldItemSig": M(cl("itemStack", cls_ns)) if cl("itemStack", cls_ns) else None,
        "itemStackClass": cl("itemStack", cls_ns),
        "itemGetItem": srg_name(R["itemGet"]),
        "itemGetItemSig": M(cl("item", cls_ns)) if cl("item", cls_ns) else None,
        "itemBlockClass": cl("itemBlock", cls_ns),
    }
    if m["typeOfHitField"]:
        m["typeOfHitSig"] = L(cl("type", cls_ns))
    elif m["typeOfHitGetter"]:
        m["typeOfHitSig"] = "()" + L(cl("type", cls_ns))
    if m["entityHitField"]:
        m["entityHitSig"] = L(cl("entity", cls_ns))
    elif m["entityHitGetter"]:
        m["entityHitSig"] = "()" + L(cl("entity", cls_ns))
    maps.append(m)

    # ---- mojang (官方名, 仅 era B) ----
    if era == "B":
        mc_obf = cl("mc", "obf")
        m = {
            "name": "mojang" + ver.replace(".", ""),
            "mcClass": cl("mc", "mojang"),
            "mcSig": M(cl("mc", "mojang")),
            "getMinecraft": moj_method(mc_obf, ["getInstance", "getMinecraft"]),
            "thePlayerField": moj_field(mc_obf, R["playerFd"]["obfn"]) if R["playerFd"] else None,
            "playerFieldSig": L(cl("player", "mojang")),
            "mopField": moj_field(mc_obf, R["mopFd"]["obfn"]) if R["mopFd"] else None,
            "mopFieldSig": L(cl("mop", "mojang")),
            "mopClass": cl("mop", "mojang"),
            "typeOfHitField": None,
            "typeOfHitGetter": moj_method(cl("mop", "obf"), ["getType"]),
            "typeOfHitSig": "()" + L(cl("type", "mojang")),
            "entityHitClass": cl("entityHitCls", "mojang"),
            "entityHitField": None,
            "entityHitGetter": moj_method(cl("entityHitCls", "obf"), ["getEntity"]),
            "entityHitSig": "()" + L(cl("entity", "mojang")),
            "typeClass": cl("type", "mojang"),
            "entityConstField": "ENTITY" if R["ent_ok"] else None,
            "entityConstAlt": None,
            "entityConstSig": L(cl("type", "mojang")),
            "entityClass": cl("entity", "mojang"),
            "canAttackWithItem": None,
            "isAliveMethod": moj_method(cl("entity", "obf"), ["isAlive"]),
            "isAttackable": moj_method(cl("entity", "obf"), ["isAttackable"]),
            "livingClass": cl("living", "mojang"),
            "heldItemGetter": moj_method(cl("living", "obf"),
                                         ["getMainHandItem", "getHeldItemMainhand", "getHeldItem"]),
            "heldItemSig": M(cl("itemStack", "mojang")),
            "itemStackClass": cl("itemStack", "mojang"),
            "itemGetItem": moj_method(cl("itemStack", "obf"), ["getItem"]),
            "itemGetItemSig": M(cl("item", "mojang")),
            "itemBlockClass": cl("itemBlock", "mojang"),
        }
        maps.append(m)

    # ---- intermediary (Fabric 运行时, 1.14.4+) ----
    if C["mc"] and len(C["mc"]) > 5 and C["mc"][5]:
        m = {
            "name": "intermediary" + ver.replace(".", ""),
            "mcClass": cl("mc", "intermediary"),
            "mcSig": M(cl("mc", "intermediary")) if cl("mc", "intermediary") else None,
            "getMinecraft": int_member(R["getMc"], False),
            "thePlayerField": int_member(R["playerFd"], True),
            "playerFieldSig": L(cl("player", "intermediary")) if cl("player", "intermediary") else None,
            "mopField": int_member(R["mopFd"], True),
            "mopFieldSig": L(cl("mop", "intermediary")) if cl("mop", "intermediary") else None,
            "mopClass": cl("mop", "intermediary"),
            "typeOfHitField": None,
            "typeOfHitGetter": int_member(R["typeGet"], False),
            "typeOfHitSig": ("()" + L(cl("type", "intermediary"))) if cl("type", "intermediary") else None,
            "entityHitClass": cl("entityHitCls", "intermediary"),
            "entityHitField": None,
            "entityHitGetter": int_member(R["entGet"], False),
            "entityHitSig": ("()" + L(cl("entity", "intermediary"))) if cl("entity", "intermediary") else None,
            "typeClass": cl("type", "intermediary"),
            "entityConstField": R["ent_int"],
            "entityConstAlt": None,
            "entityConstSig": L(cl("type", "intermediary")) if cl("type", "intermediary") else None,
            "entityClass": cl("entity", "intermediary"),
            "canAttackWithItem": int_member(R["canAttack"], False),
            "isAliveMethod": int_member(R["isAlive"], False),
            "isAttackable": int_member(R["isAttackable"], False),
            "livingClass": cl("living", "intermediary"),
            "heldItemGetter": int_member(R["held"], False),
            "heldItemSig": M(cl("itemStack", "intermediary")) if cl("itemStack", "intermediary") else None,
            "itemStackClass": cl("itemStack", "intermediary"),
            "itemGetItem": int_member(R["itemGet"], False),
            "itemGetItemSig": M(cl("item", "intermediary")) if cl("item", "intermediary") else None,
            "itemBlockClass": cl("itemBlock", "intermediary"),
        }
        maps.append(m)

    return maps


FIELD_ORDER = [
    "name", "mcClass", "mcSig", "getMinecraft", "thePlayerField", "playerFieldSig",
    "mopField", "mopFieldSig", "mopClass", "typeOfHitField", "typeOfHitGetter",
    "typeOfHitSig", "entityHitClass", "entityHitField", "entityHitGetter", "entityHitSig",
    "typeClass", "entityConstField", "entityConstAlt", "entityConstSig", "entityClass",
    "canAttackWithItem", "isAliveMethod", "isAttackable", "livingClass", "heldItemGetter",
    "heldItemSig", "itemStackClass", "itemGetItem", "itemGetItemSig", "itemBlockClass",
]


def emit_header(all_maps):
    n_map = sum(len(maps) for _, maps in all_maps)
    lines = [
        "// 自动生成 by tools/gen_maps.py —— 勿手改",
        "// 覆盖 %d 个版本, %d 张映射表 (vanilla/forge/mojang/intermediary)" % (len(all_maps), n_map),
        "// 集成: 在 MCCombatStatusJni.cpp 的 JniMap 结构定义之后 #include \"mc_maps_generated.h\",",
        "//       再把 kAllMaps[] / kMapCount 替换为 kGenMaps[] / kGenMapCount。",
        "#ifndef MC_MAPS_GENERATED_H",
        "#define MC_MAPS_GENERATED_H",
        "",
        "static const JniMap kGenMaps[] = {",
    ]
    for ver, maps in all_maps:
        lines.append("    // ==== %s ====" % ver)
        for m in maps:
            lines.append("    {  // %s" % m["name"])
            for f in FIELD_ORDER:
                lines.append("        %s,  // %s" % (cstr(m.get(f)), f))
            lines.append("    },")
    lines += [
        "};",
        "static const int kGenMapCount = (int)(sizeof(kGenMaps)/sizeof(kGenMaps[0]));",
        "",
        "#endif // MC_MAPS_GENERATED_H",
        "",
    ]
    return "\n".join(lines)


def report(all_maps, R_by_ver):
    out = []
    for ver, maps in all_maps:
        R = R_by_ver[ver]
        out.append("==== %s  shape=%s era=%s  missing=%s ====" % (
            ver, R["shape"], R["era"], R["missing_classes"] or "ok"))
        for m in maps:
            out.append("  [%s]" % m["name"])
            for f in ["mcClass", "getMinecraft", "thePlayerField", "mopField",
                      "typeOfHitGetter", "typeOfHitField", "entityHitClass",
                      "entityHitGetter", "entityHitField", "typeClass",
                      "entityConstField", "entityConstAlt", "entityClass",
                      "canAttackWithItem", "isAliveMethod", "isAttackable",
                      "livingClass", "heldItemGetter", "itemStackClass",
                      "itemGetItem", "itemBlockClass"]:
                out.append("      %-16s = %s" % (f, m.get(f)))
    return "\n".join(out)


def query(ver, term):
    """跨命名空间查一个名字 (类/字段/方法), 打印完整行。用于取代 grep lzma/mcp_config。"""
    v = Version(ver)
    hits = 0
    for fname, rows in (("classes.csv", v.classes), ("fields.csv", v.fields),
                        ("methods.csv", v.methods)):
        for row in rows[1:]:
            if any(term in c for c in row):
                print("[%s] %s" % (fname, ",".join(row)))
                hits += 1
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--versions", nargs="*", default=None, help="只处理指定版本 (前缀匹配)")
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--query", nargs=2, metavar=("VERSION", "TERM"),
                    help="跨命名空间查一个名字 (类/字段/方法子串)")
    args = ap.parse_args()

    if args.query:
        ver, term = args.query
        n = query(ver, term)
        print("--- %d 条匹配 ---" % n)
        return

    def vkey(s):
        return [int(x) for x in s.split(".")]

    vers = sorted([d for d in os.listdir(BASE)
                   if os.path.isdir(os.path.join(BASE, d))
                   and d[0].isdigit()],
                  key=vkey)
    if args.versions:
        vers = [v for v in vers if any(v == a or v.startswith(a) for a in args.versions)]

    all_maps, R_by_ver = [], {}
    for ver in vers:
        try:
            v = Version(ver)
            R = resolve(v)
            maps = build_maps(v, R)
            all_maps.append((ver, maps))
            R_by_ver[ver] = R
        except Exception as e:
            print("!! %s 解析失败: %r" % (ver, e), file=sys.stderr)

    if args.report:
        print(report(all_maps, R_by_ver))
    if args.emit:
        with open(OUT_H, "w", encoding="utf-8") as f:
            f.write(emit_header(all_maps))
        print("已生成: %s (%d 版本, %d 张表)" % (OUT_H, len(all_maps),
              sum(len(m) for _, m in all_maps)))


if __name__ == "__main__":
    main()
