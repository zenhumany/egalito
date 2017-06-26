#include "usedef.h"
#include "slicingtree.h"
#include "slicingmatch.h"
#include "chunk/concrete.h"
#include "instr/assembly.h"
#include "instr/isolated.h"

#include <assert.h>
#include "chunk/dump.h"
#include "log/log.h"

void DefList::set(int reg, TreeNode *tree) {
    list[reg] = tree;
}

TreeNode *DefList::get(int reg) const {
    auto it = list.find(reg);
    if(it != list.end()) {
        return it->second;
    }
    return nullptr;
}

void DefList::dump() const {
    for(auto it = list.cbegin(); it != list.cend(); it++) {
        LOG0(9, "R" << std::dec << it->first << ":  ");
        if(auto tree = it->second) {
            IF_LOG(9) tree->print(TreePrinter(0, 0));
        }
        LOG(9, "");
    }
}


void RefList::set(int reg, UDState *origin) {
    list[reg] = {origin};
}

void RefList::add(int reg, UDState *origin) {
    auto exist = addIfExist(reg, origin);
    if(!exist) {
        list[reg].push_back(origin);
    }
}

bool RefList::addIfExist(int reg, UDState *origin) {
    bool found = false;
    auto it = list.find(reg);
    if(it != list.end()) {
        bool duplicate = false;
        for(auto s : it->second) {
            if(s == origin) {
                duplicate = true;
                break;
            }
        }
        if(!duplicate) {
            it->second.push_back(origin);
        }
        found = true;
    }
    return found;
}

void RefList::del(int reg) {
    list.erase(reg);
}

void RefList::clear() {
    list.clear();
}

const std::vector<UDState *> *RefList::get(int reg) const {
    auto it = list.find(reg);
    if(it != list.end()) {
        return &it->second;
    }
    return nullptr;
}

void RefList::dump() const {
    for(auto it = list.cbegin(); it != list.cend(); it++) {
        LOG0(9, "R" << std::dec << it->first << " <[");
        for(auto o : it->second) {
            LOG0(9, " 0x" << std::hex << o->getInstruction()->getAddress());
        }
        LOG(9, " ]");
    }
}


void MemOriginList::set(TreeNode *place, UDState *origin) {
    bool found = false;
    MemLocation m1(place);
    for(auto it = list.rbegin(); it != list.rend(); ++it) {
        MemLocation m2(it->place);
        if(m1 == m2) {
            if(!found) {
                found = true;
                *it = MemOrigin(place, origin);
            }
            else {
                *it = list.back();
                list.pop_back();
            }
        }
    }
    if(!found) {
        list.emplace_back(place, origin);
    }
}

void MemOriginList::add(TreeNode *place, UDState *origin) {
    bool duplicate = false;
    MemLocation m1(place);
    for(const auto& mem : list) {
        if(mem.origin == origin) {
            MemLocation m2(mem.place);
            if(m1 == m2) {
                duplicate = true;
                break;
            }
        }
    }
    if(!duplicate) {
        list.emplace_back(place, origin);
    }
}

void MemOriginList::addList(const MemOriginList& other) {
    for(auto it = other.cbegin(); it != other.cend(); ++it) {
        add(it->place, it->origin);
    }
}

void MemOriginList::del(TreeNode *tree) {
    MemLocation m1(tree);
    for(auto it = list.rbegin(); it != list.rend(); ++it) {
        MemLocation m2(it->place);
        if(m1 == m2) {
            *it = list.back();
            list.pop_back();
        }
    }
}

void MemOriginList::clear() {
    list.clear();
}

void MemOriginList::dump() const {
    for(const auto &m : list) {
        IF_LOG(9) m.place->print(TreePrinter(0, 0));
        LOG(9, " : 0x"
             << std::hex << m.origin->getInstruction()->getAddress());
    }
}

void RegState::dumpRegState() const {
    LOG(9, "reg definition list:");
    regList.dump();

    LOG(9, "reg reference list:");
    regRefList.dump();
}

void RegMemState::dumpMemState() const {
    LOG(9, "mem definition list:");
    memList.dump();

    LOG(9, "mem reference list:");
    memRefList.dump();
}


UseDefConfiguration::UseDefConfiguration(int level,
    ControlFlowGraph *cfg, const std::vector<int> &idList)
    : level(level), cfg(cfg) {

    for(auto id : idList) {
        enabled[id] = true;
    }
}

bool UseDefConfiguration::isEnabled(int id) const {
    auto it = enabled.find(id);
    if(it != enabled.end()) {
        return true;
    }
    return false;
}


void UseDefWorkSet::transitionTo(ControlFlowNode *node) {
    regSet = &nodeExposedRegSetList[node->getID()];
    memSet = &nodeExposedMemSetList[node->getID()];
    regSet->clear();
    memSet->clear();
    for(auto link : node->backwardLinks()) {
        for(auto mr : nodeExposedRegSetList[link.getID()]) {
            for(auto o : mr.second) {
                addToRegSet(mr.first, o);
            }
        }

        memSet->addList(nodeExposedMemSetList[link.getID()]);
    }
}

void UseDefWorkSet::copyFromMemSetFor(
    UDState *state, int reg, TreeNode *place) {

    MemLocation loc1(place);
    for(auto &m : *memSet) {
        MemLocation loc2(m.place);
        if(loc1 == loc2) {
            state->addMemRef(reg, m.origin);
        }
    }
}

void UseDefWorkSet::dumpSet() const {
    LOG(9, "REG SET");
    regSet->dump();

    LOG(9, "MEM SET");
    memSet->dump();
}


const std::map<int, UseDef::HandlerType> UseDef::handlers = {
    {ARM64_INS_ADD,     &UseDef::fillAddOrSub},
    {ARM64_INS_ADR,     &UseDef::fillAdr},
    {ARM64_INS_ADRP,    &UseDef::fillAdrp},
    {ARM64_INS_AND,     &UseDef::fillAnd},
    {ARM64_INS_B,       &UseDef::fillB},
    {ARM64_INS_BL,      &UseDef::fillBl},
    {ARM64_INS_BLR,     &UseDef::fillBlr},
    {ARM64_INS_BR,      &UseDef::fillBr},
    {ARM64_INS_CBZ,     &UseDef::fillCbz},
    {ARM64_INS_CBNZ,    &UseDef::fillCbnz},
    {ARM64_INS_CMP,     &UseDef::fillCmp},
    {ARM64_INS_CSEL,    &UseDef::fillCsel},
    {ARM64_INS_LDAXR,   &UseDef::fillLdaxr},
    {ARM64_INS_LDP,     &UseDef::fillLdp},
    {ARM64_INS_LDR,     &UseDef::fillLdr},
    {ARM64_INS_LDRH,    &UseDef::fillLdrh},
    {ARM64_INS_LDRB,    &UseDef::fillLdrb},
    {ARM64_INS_LDRSW,   &UseDef::fillLdrsw},
    {ARM64_INS_LDRSH,   &UseDef::fillLdrsh},
    {ARM64_INS_LDRSB,   &UseDef::fillLdrsb},
    {ARM64_INS_LDUR,    &UseDef::fillLdur},
    {ARM64_INS_LSL,     &UseDef::fillLsl},
    {ARM64_INS_MOV,     &UseDef::fillMov},
    {ARM64_INS_MRS,     &UseDef::fillMrs},
    {ARM64_INS_NOP,     &UseDef::fillNop},
    {ARM64_INS_RET,     &UseDef::fillRet},
    {ARM64_INS_STP,     &UseDef::fillStp},
    {ARM64_INS_STR,     &UseDef::fillStr},
    {ARM64_INS_STRB,    &UseDef::fillStrb},
    {ARM64_INS_STRH,    &UseDef::fillStrh},
    {ARM64_INS_SUB,     &UseDef::fillAddOrSub},
    {ARM64_INS_SXTW,    &UseDef::fillSxtw},
};

void UseDef::analyze(const std::vector<std::vector<int>>& order) {
    LOG(10, "full order:");
    for(auto o : order) {
        LOG0(10, "{");
        for(auto n : o) {
            LOG0(10, " " << std::dec << n);
        }
        LOG0(10, " }");
    }
    LOG(10, "");

    for(auto o : order) {
        analyzeGraph(o);
        if(o.size() > 1) {
            analyzeGraph(o);
        }
    }
}

void UseDef::analyzeGraph(const std::vector<int>& order) {
    LOG(10, "order:");
    for(auto o : order) {
        LOG0(10, " " << std::dec << o);
    }
    LOG(10, "");

    for(auto nodeId : order) {
        auto node = config->getCFG()->get(nodeId);
        work->transitionTo(node);

        auto blockList = CIter::children(node->getBlock());

        for(auto it = blockList.begin(); it != blockList.end(); ++it) {
            auto state = work->getState(*it);

            LOG(9, "analyzing state @ 0x" << std::hex
                << state->getInstruction()->getAddress());

            if(dynamic_cast<LiteralInstruction *>(
                state->getInstruction()->getSemantic())) {
                continue;
            }

            fillState(state);
        }

        LOG(9, "");
        LOG(9, "final set for node " << std::dec << nodeId);
        work->dumpSet();
        LOG(9, "");
    }
}

bool UseDef::callIfEnabled(UDState *state, Assembly *assembly) {
    bool handled = false;
    if(config->isEnabled(assembly->getId())) {
        auto f = handlers.find(assembly->getId())->second;
        (this->*f)(state, assembly);
        handled = true;
    }
    else {
        LOG(9, "handler disabled (or not found): " << assembly->getMnemonic());
        LOG(9, "mode: " << assembly->getAsmOperands()->getMode());
    }

    return handled;
}

void UseDef::fillState(UDState *state) {
    ChunkDumper dumper;
    state->getInstruction()->accept(&dumper);

    auto assembly = state->getInstruction()->getSemantic()->getAssembly();
    if(assembly->getId() == ARM64_INS_AT) {
        throw "AT should be an alias for SYS";
    }

    bool handled = callIfEnabled(state, assembly);
    if(handled) {
        state->dumpState();
        work->dumpSet();
    }
}

void UseDef::defReg(UDState *state, int reg, TreeNode *tree) {
    if(reg != -1) {
        state->addRegDef(reg, tree);
        work->setAsRegSet(reg, state);
    }
}

void UseDef::useReg(UDState *state, int reg) {
    auto origins = work->getRegSet(reg);
    if(origins) {
        for(auto o : *origins) {
            state->addRegRef(reg, o);
        }
    }
}

void UseDef::defMem(UDState *state, TreeNode *place, int reg) {
    state->addMemDef(reg, place);
    work->setAsMemSet(place, state);
}

void UseDef::useMem(UDState *state, TreeNode *place, int reg) {
    work->copyFromMemSetFor(state, reg, place);
}

TreeNode *UseDef::shiftExtend(TreeNode *tree, arm64_shifter type,
    unsigned int value) {

    switch(type) {
    case ARM64_SFT_LSL:
        tree = TreeFactory::instance().make<TreeNodeLogicalShiftLeft>(tree,
            TreeFactory::instance().make<TreeNodeConstant>(value));
        break;
    case ARM64_SFT_MSL:
        throw "msl";
        break;
    case ARM64_SFT_LSR:
        tree = TreeFactory::instance().make<TreeNodeLogicalShiftRight>(tree,
            TreeFactory::instance().make<TreeNodeConstant>(value));
        break;
    case ARM64_SFT_ASR:
        tree = TreeFactory::instance().make<TreeNodeArithmeticShiftRight>(tree,
            TreeFactory::instance().make<TreeNodeConstant>(value));
        break;
    case ARM64_SFT_ROR:
        tree = TreeFactory::instance().make<TreeNodeRotateRight>(tree,
            TreeFactory::instance().make<TreeNodeConstant>(value));
        break;
    case ARM64_SFT_INVALID:
    default:
        break;
    }

    return tree;
}

void UseDef::fillImm(UDState *state, Assembly *assembly) {
    throw "NYI: fillImm";
}

void UseDef::fillReg(UDState *state, Assembly *assembly) {
    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    useReg(state, reg0);
}

void UseDef::fillRegToReg(UDState *state, Assembly *assembly) {
    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    auto op1 = assembly->getAsmOperands()->getOperands()[1].reg;
    int reg1 = AARCH64GPRegister::convertToPhysical(op1);
    size_t width1 = AARCH64GPRegister::getWidth(reg1, op1);

    useReg(state, reg1);
    auto tree = TreeFactory::instance().make<
        TreeNodePhysicalRegister>(reg1, width1);

    defReg(state, reg0, tree);
}

void UseDef::fillMemToReg(UDState *state, Assembly *assembly, size_t width) {
    assert(!assembly->isPostIndex());

    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    size_t width0 = AARCH64GPRegister::getWidth(reg0, op0);

    auto mem = assembly->getAsmOperands()->getOperands()[1].mem;
    auto base = AARCH64GPRegister::convertToPhysical(mem.base);
    size_t widthB = AARCH64GPRegister::getWidth(base, mem.base);
    useReg(state, base);

    if(mem.index != INVALID_REGISTER) {
        LOG(9, "NYI: index register");
        defReg(state,
            reg0,
            TreeFactory::instance().make<
                TreeNodePhysicalRegister>(reg0, width0));
        return;
    }

    auto memTree = TreeFactory::instance().make<TreeNodeAddition>(
        TreeFactory::instance().make<TreeNodePhysicalRegister>(base, widthB),
        TreeFactory::instance().make<TreeNodeConstant>(mem.disp));
    useMem(state, memTree, reg0);

    if(assembly->isPreIndex()) {
        defReg(state, base, memTree);
    }

    auto derefTree
        = TreeFactory::instance().make<TreeNodeDereference>(memTree, width);
    defReg(state, reg0, derefTree);
}

void UseDef::fillImmToReg(UDState *state, Assembly *assembly) {
    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);

    auto op1 = assembly->getAsmOperands()->getOperands()[1].imm;
    TreeNode *tree1 = nullptr;
    if(assembly->getId() == ARM64_INS_ADR
        || assembly->getId() == ARM64_INS_ADRP) {

        tree1 = TreeFactory::instance().make<TreeNodeAddress>(op1);
    }
    else {
        tree1 = TreeFactory::instance().make<TreeNodeConstant>(op1);
    }
    defReg(state, reg0, tree1);
}

void UseDef::fillRegRegToReg(UDState *state, Assembly *assembly) {
    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    auto op1 = assembly->getAsmOperands()->getOperands()[1].reg;
    int reg1 = AARCH64GPRegister::convertToPhysical(op1);
    size_t width1 = AARCH64GPRegister::getWidth(reg1, op1);
    auto op2 = assembly->getAsmOperands()->getOperands()[2].reg;
    int reg2 = AARCH64GPRegister::convertToPhysical(op2);
    size_t width2 = AARCH64GPRegister::getWidth(reg2, op1);

    useReg(state, reg1);
    useReg(state, reg2);

    TreeNode *reg1tree
        = TreeFactory::instance().make<TreeNodePhysicalRegister>(reg1, width1);
    TreeNode *reg2tree
        = TreeFactory::instance().make<TreeNodePhysicalRegister>(reg2, width2);

    auto shift = assembly->getAsmOperands()->getOperands()[2].shift;
    reg2tree = shiftExtend(reg2tree, shift.type, shift.value);

    TreeNode *tree = nullptr;
    switch(assembly->getId()) {
    case ARM64_INS_ADD:
        tree = TreeFactory::instance().make<
            TreeNodeAddition>(reg1tree, reg2tree);
        break;
    case ARM64_INS_AND:
        tree = TreeFactory::instance().make<
            TreeNodeAnd>(reg1tree, reg2tree);
        break;
    case ARM64_INS_SUB:
        tree = TreeFactory::instance().make<
            TreeNodeSubtraction>(reg1tree, reg2tree);
        break;
    default:
        tree = nullptr;
        LOG(9, "NYI: " << assembly->getMnemonic());
        break;
    }
    defReg(state, reg0, tree);
}

void UseDef::fillMemImmToReg(UDState *state, Assembly *assembly) {
    assert(assembly->isPostIndex());

    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);

    auto mem = assembly->getAsmOperands()->getOperands()[1].mem;
    auto base = AARCH64GPRegister::convertToPhysical(mem.base);
    size_t widthB = AARCH64GPRegister::getWidth(base, mem.base);
    useReg(state, base);

    auto baseTree
        = TreeFactory::instance().make<TreeNodePhysicalRegister>(base, widthB);

    assert(mem.index == INVALID_REGISTER);
    assert(mem.disp == 0);

    size_t width = (assembly->getBytes()[3] & 0b01000000) ? 8 : 4;
    auto memTree = TreeFactory::instance().make<TreeNodeAddition>(
        baseTree,
        TreeFactory::instance().make<TreeNodeConstant>(0));
    useMem(state, memTree, reg0);

    auto derefTree
        = TreeFactory::instance().make<TreeNodeDereference>(memTree, width);
    defReg(state, reg0, derefTree);

    auto imm = assembly->getAsmOperands()->getOperands()[2].imm;
    auto wbTree = TreeFactory::instance().make<TreeNodeAddition>(
        baseTree,
        TreeFactory::instance().make<TreeNodeConstant>(imm));
    defReg(state, base, wbTree);
}

void UseDef::fillRegToMem(UDState *state, Assembly *assembly, size_t width) {
    assert(!assembly->isPostIndex());

    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    useReg(state, reg0);

    auto mem = assembly->getAsmOperands()->getOperands()[1].mem;
    auto base = AARCH64GPRegister::convertToPhysical(mem.base);
    size_t widthB = AARCH64GPRegister::getWidth(base, mem.base);
    useReg(state, base);

    if(mem.index != INVALID_REGISTER) {
        LOG(9, "NYI: index register");
        return;
    }

    auto memTree = TreeFactory::instance().make<TreeNodeAddition>(
        TreeFactory::instance().make<TreeNodePhysicalRegister>(base, widthB),
        TreeFactory::instance().make<TreeNodeConstant>(mem.disp));

    if(assembly->isPreIndex()) {
        defReg(state, base, memTree);
    }

    defMem(state, memTree, reg0);
}

void UseDef::fillRegImmToReg(UDState *state, Assembly *assembly) {

    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);

    auto op1 = assembly->getAsmOperands()->getOperands()[1].reg;
    int reg1 = AARCH64GPRegister::convertToPhysical(op1);
    size_t width1 = AARCH64GPRegister::getWidth(reg1, op1);
    useReg(state, reg1);

    auto regTree
        = TreeFactory::instance().make<TreeNodePhysicalRegister>(reg1, width1);

    long int imm = assembly->getAsmOperands()->getOperands()[2].imm;
    auto shift = assembly->getAsmOperands()->getOperands()[2].shift;
    TreeNode *immTree
        = TreeFactory::instance().make<TreeNodeConstant>(imm);

    immTree = shiftExtend(immTree, shift.type, shift.value);

    TreeNode *tree = nullptr;
    switch(assembly->getId()) {
    case ARM64_INS_ADD:
        tree = TreeFactory::instance().make<
            TreeNodeAddition>(regTree, immTree);
        break;
    case ARM64_INS_AND:
        tree = TreeFactory::instance().make<
            TreeNodeAnd>(regTree, immTree);
        break;
    case ARM64_INS_SUB:
        tree = TreeFactory::instance().make<
            TreeNodeSubtraction>(regTree, immTree);
        break;
    default:
        tree = nullptr;
        LOG(9, "NYI: " << assembly->getMnemonic());
        break;
    }
    defReg(state, reg0, tree);
}

void UseDef::fillMemToRegReg(UDState *state, Assembly *assembly) {

    assert(!assembly->isPostIndex());

    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);

    auto op1 = assembly->getAsmOperands()->getOperands()[1].reg;
    int reg1 = AARCH64GPRegister::convertToPhysical(op1);

    auto mem = assembly->getAsmOperands()->getOperands()[2].mem;
    auto base = AARCH64GPRegister::convertToPhysical(mem.base);
    size_t widthB = AARCH64GPRegister::getWidth(base, mem.base);
    useReg(state, base);

    assert(mem.index == INVALID_REGISTER);
    auto disp = mem.disp;
    auto dispTree = TreeFactory::instance().make<TreeNodeConstant>(disp);

    auto memTree = TreeFactory::instance().make<TreeNodeAddition>(
        TreeFactory::instance().make<TreeNodePhysicalRegister>(base, widthB),
        dispTree);
    if(assembly->isPreIndex()) {
        defReg(state, base, memTree);
    }

    size_t width = (assembly->getBytes()[3] & 0b10000000) ? 8 : 4;
    auto memTree0 = TreeFactory::instance().make<TreeNodeAddition>(
        memTree,
        TreeFactory::instance().make<TreeNodeConstant>(0));
    auto memTree1 = TreeFactory::instance().make<TreeNodeAddition>(
        memTree,
        TreeFactory::instance().make<TreeNodeConstant>(width));
    useMem(state, memTree0, reg0);
    useMem(state, memTree1, reg1);

    auto derefTree0
        = TreeFactory::instance().make<TreeNodeDereference>(memTree0, width);
    auto derefTree1
        = TreeFactory::instance().make<TreeNodeDereference>(memTree1, width);
    defReg(state, reg0, derefTree0);
    defReg(state, reg1, derefTree1);
}

void UseDef::fillRegRegToMem(UDState *state, Assembly *assembly) {
    assert(!assembly->isPostIndex());

    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    auto op1 = assembly->getAsmOperands()->getOperands()[1].reg;
    int reg1 = AARCH64GPRegister::convertToPhysical(op1);

    useReg(state, reg0);
    useReg(state, reg1);

    auto mem = assembly->getAsmOperands()->getOperands()[2].mem;
    auto base = AARCH64GPRegister::convertToPhysical(mem.base);
    size_t widthB = AARCH64GPRegister::getWidth(base, mem.base);
    useReg(state, base);
    assert(mem.index == INVALID_REGISTER);
    auto disp = mem.disp;
    auto dispTree = TreeFactory::instance().make<TreeNodeConstant>(disp);

    auto memTree = TreeFactory::instance().make<TreeNodeAddition>(
        TreeFactory::instance().make<TreeNodePhysicalRegister>(base, widthB),
        dispTree);
    if(assembly->isPreIndex()) {
        defReg(state, base, memTree);
    }

    size_t width = (assembly->getBytes()[3] & 0b10000000) ? 8 : 4;
    auto memTree0 = TreeFactory::instance().make<TreeNodeAddition>(
        memTree,
        TreeFactory::instance().make<TreeNodeConstant>(0));
    auto memTree1 = TreeFactory::instance().make<TreeNodeAddition>(
        memTree,
        TreeFactory::instance().make<TreeNodeConstant>(width));

    defMem(state, memTree0, reg0);
    defMem(state, memTree1, reg1);
}

void UseDef::fillRegRegImmToMem(UDState *state, Assembly *assembly) {

    assert(assembly->isPostIndex());

    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    auto op1 = assembly->getAsmOperands()->getOperands()[1].reg;
    int reg1 = AARCH64GPRegister::convertToPhysical(op1);
    useReg(state, reg0);
    useReg(state, reg1);

    auto mem = assembly->getAsmOperands()->getOperands()[2].mem;
    auto base = AARCH64GPRegister::convertToPhysical(mem.base);
    size_t widthB = AARCH64GPRegister::getWidth(base, mem.base);
    useReg(state, base);

    auto baseTree
        = TreeFactory::instance().make<TreeNodePhysicalRegister>(base, widthB);

    assert(mem.index == INVALID_REGISTER);
    assert(mem.disp == 0);

    size_t width = (assembly->getBytes()[3] & 0b10000000) ? 8 : 4;
    auto memTree0 = TreeFactory::instance().make<TreeNodeAddition>(
        baseTree,
        TreeFactory::instance().make<TreeNodeConstant>(0));
    auto memTree1 = TreeFactory::instance().make<TreeNodeAddition>(
        baseTree,
        TreeFactory::instance().make<TreeNodeConstant>(width));
    defMem(state, memTree0, reg0);
    defMem(state, memTree1, reg1);

    auto imm = assembly->getAsmOperands()->getOperands()[3].imm;
    auto wbTree = TreeFactory::instance().make<TreeNodeAddition>(
        baseTree,
        TreeFactory::instance().make<TreeNodeConstant>(imm));
    defReg(state, base, wbTree);
}

void UseDef::fillMemImmToRegReg(UDState *state, Assembly *assembly) {
    assert(assembly->isPostIndex());

    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    auto op1 = assembly->getAsmOperands()->getOperands()[1].reg;
    int reg1 = AARCH64GPRegister::convertToPhysical(op1);

    auto mem = assembly->getAsmOperands()->getOperands()[2].mem;
    auto base = AARCH64GPRegister::convertToPhysical(mem.base);
    size_t widthB = AARCH64GPRegister::getWidth(base, mem.base);
    useReg(state, base);

    auto baseTree
        = TreeFactory::instance().make<TreeNodePhysicalRegister>(base, widthB);

    assert(mem.index == INVALID_REGISTER);
    assert(mem.disp == 0);

    size_t width = (assembly->getBytes()[3] & 0b10000000) ? 8 : 4;
    auto memTree0 = TreeFactory::instance().make<TreeNodeAddition>(
        baseTree,
        TreeFactory::instance().make<TreeNodeConstant>(0));
    auto memTree1 = TreeFactory::instance().make<TreeNodeAddition>(
        baseTree,
        TreeFactory::instance().make<TreeNodeConstant>(width));
    useMem(state, memTree0, reg0);
    useMem(state, memTree1, reg1);

    auto derefTree0
        = TreeFactory::instance().make<TreeNodeDereference>(memTree0, width);
    auto derefTree1
        = TreeFactory::instance().make<TreeNodeDereference>(memTree1, width);
    defReg(state, reg0, derefTree0);
    defReg(state, reg1, derefTree1);

    auto imm = assembly->getAsmOperands()->getOperands()[3].imm;
    auto wbTree = TreeFactory::instance().make<TreeNodeAddition>(
        baseTree,
        TreeFactory::instance().make<TreeNodeConstant>(imm));
    defReg(state, base, wbTree);
}

void UseDef::fillCompareImmThenJump(UDState *state, Assembly *assembly) {
    // CBZ, CBNZ do not update NZCV, but this information may be useful for
    // jumptable detection
}

void UseDef::fillCondJump(UDState *state, Assembly *assembly) {
}


void UseDef::fillAddOrSub(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_REG_IMM) {
        fillRegImmToReg(state, assembly);
    }
    else if(mode == AssemblyOperands::MODE_REG_REG_REG) {
        fillRegRegToReg(state, assembly);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillAdr(UDState *state, Assembly *assembly) {
    fillImmToReg(state, assembly);
}
void UseDef::fillAdrp(UDState *state, Assembly *assembly) {
    fillImmToReg(state, assembly);
}
void UseDef::fillAnd(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    assert(mode == AssemblyOperands::MODE_REG_REG_IMM);

    fillRegImmToReg(state, assembly);
}
void UseDef::fillB(UDState *state, Assembly *assembly) {
}
void UseDef::fillBl(UDState *state, Assembly *assembly) {
    for(int i = 0; i < 8; i++) {
        useReg(state, i);
        defReg(state, i, nullptr);
    }
}
void UseDef::fillBlr(UDState *state, Assembly *assembly) {
    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    useReg(state, reg0);

    for(int i = 0; i < 8; i++) {
        useReg(state, i);
        defReg(state, i, nullptr);
    }
}
void UseDef::fillBr(UDState *state, Assembly *assembly) {
    fillReg(state, assembly);
}
void UseDef::fillCbz(UDState *state, Assembly *assembly) {
    fillCompareImmThenJump(state, assembly);
}
void UseDef::fillCbnz(UDState *state, Assembly *assembly) {
    fillCompareImmThenJump(state, assembly);
}
void UseDef::fillCmp(UDState *state, Assembly *assembly) {
    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    size_t width0 = AARCH64GPRegister::getWidth(reg0, op0);
    auto imm = assembly->getAsmOperands()->getOperands()[1].imm;
    auto tree = TreeFactory::instance().make<TreeNodeComparison>(
        TreeFactory::instance().make<TreeNodePhysicalRegister>(reg0, width0),
        TreeFactory::instance().make<TreeNodeConstant>(imm));
    defReg(state, AARCH64GPRegister::NZCV, tree);
}
void UseDef::fillCsel(UDState *state, Assembly *assembly) {
    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    size_t width0 = AARCH64GPRegister::getWidth(reg0, op0);
    defReg(state,
        reg0,
        TreeFactory::instance().make<TreeNodePhysicalRegister>(reg0, width0));
    LOG(9, "NYI: " << assembly->getMnemonic());
}
void UseDef::fillLdaxr(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_MEM) {
        size_t width = (assembly->getBytes()[3] & 0b01000000) ? 8 : 4;
        fillMemToReg(state, assembly, width);
    }
    else {
        throw "unknown mode for LDAXR";
    }
}
void UseDef::fillLdp(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_REG_MEM) {
        fillMemToRegReg(state, assembly);
    }
    else if(mode == AssemblyOperands::MODE_REG_REG_MEM_IMM) {
        fillMemImmToRegReg(state, assembly);
    }
    else {
        throw "unknown mode for LDP";
    }
}
void UseDef::fillLdr(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_MEM) {
        size_t width = (assembly->getBytes()[3] & 0b01000000) ? 8 : 4;
        fillMemToReg(state, assembly, width);
    }
    else if(mode == AssemblyOperands::MODE_REG_MEM_IMM) {
        fillMemImmToReg(state, assembly);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillLdrh(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_MEM) {
        fillMemToReg(state, assembly, 2);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillLdrb(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_MEM) {
        fillMemToReg(state, assembly, 1);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillLdrsw(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_MEM) {
        fillMemToReg(state, assembly, 4);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillLdrsh(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_MEM) {
        fillMemToReg(state, assembly, 2);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillLdrsb(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_MEM) {
        fillMemToReg(state, assembly, 1);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillLdur(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_MEM) {
        size_t width = (assembly->getBytes()[3] & 0b01000000) ? 8 : 4;
        fillMemToReg(state, assembly, width);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillLsl(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_REG_IMM) {
        fillRegImmToReg(state, assembly);
    }
    else if(mode == AssemblyOperands::MODE_REG_REG_REG) {
        fillRegRegToReg(state, assembly);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillNop(UDState *state, Assembly *assembly) {
    /* Nothing to do */
}
void UseDef::fillMov(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_REG) {
        fillRegToReg(state, assembly);
    }
    else if(mode == AssemblyOperands::MODE_REG_IMM) {
        fillImmToReg(state, assembly);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillMrs(UDState *state, Assembly *assembly) {
    auto op0 = assembly->getAsmOperands()->getOperands()[0].reg;
    int reg0 = AARCH64GPRegister::convertToPhysical(op0);
    size_t width0 = AARCH64GPRegister::getWidth(reg0, op0);
    defReg(state,
        reg0,
        TreeFactory::instance().make<TreeNodePhysicalRegister>(reg0, width0));
}
void UseDef::fillRet(UDState *state, Assembly *assembly) {
    for(int i = 0; i < 8; i++) {
        useReg(state, i);
    }
}
void UseDef::fillStp(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_REG_MEM) {
        fillRegRegToMem(state, assembly);
    }
    else if(mode == AssemblyOperands::MODE_REG_REG_MEM_IMM) {
        fillRegRegImmToMem(state, assembly);
    }
    else {
        throw "unknown mode for STP";
    }
}
void UseDef::fillStr(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_MEM) {
        size_t width = (assembly->getBytes()[3] & 0b01000000) ? 8 : 4;
        fillRegToMem(state, assembly, width);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillStrb(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_MEM) {
        fillRegToMem(state, assembly, 1);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillStrh(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_MEM) {
        fillRegToMem(state, assembly, 2);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}
void UseDef::fillSxtw(UDState *state, Assembly *assembly) {
    auto mode = assembly->getAsmOperands()->getMode();
    if(mode == AssemblyOperands::MODE_REG_REG) {
        LOG(9, "NYI fully: " << assembly->getMnemonic());
        fillRegToReg(state, assembly);
    }
    else {
        LOG(9, "skipping mode " << mode);
    }
}

void MemLocation::extract(TreeNode *tree) {
    TreeCapture cap;
    if(MemoryForm::matches(tree, cap)) {
        for(size_t i = 0; i < cap.getCount(); ++i) {
            auto c = cap.get(i);
            if(auto t = dynamic_cast<TreeNodeConstant *>(c)) {
                offset += t->getValue();
            }
            else if(auto t = dynamic_cast<TreeNodePhysicalRegister *>(c)) {
                reg = t;
            }
        }
    }
}


