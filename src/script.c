#include "global.h"
#include "script.h"
#include "event_data.h"
#include "mevent.h"
#include "util.h"
#include "constants/map_scripts.h"

#define RAM_SCRIPT_MAGIC 51

enum {
    SCRIPT_MODE_STOPPED,
    SCRIPT_MODE_BYTECODE,
    SCRIPT_MODE_NATIVE,
};

enum {
    SCRIPT_ON,
    SCRIPT_WAIT,
    SCRIPT_OFF,
};

extern const u8* gRamScriptRetAddr;

static u8 sScriptContext1Status;
static struct ScriptContext sScriptContext1;
static struct ScriptContext sScriptContext2;
static bool8 sScriptContext2Enabled;

extern ScrCmdFunc gScriptCmdTable[];
extern ScrCmdFunc gScriptCmdTableEnd[];
extern void *gNullScriptPtr;

void InitScriptContext(struct ScriptContext *ctx, void *cmdTable, void *cmdTableEnd)
{
    u32 i;

    ctx->mode = SCRIPT_MODE_STOPPED;
    ctx->scriptPtr = NULL;
    ctx->stackDepth = 0;
    ctx->nativePtr = NULL;
    ctx->cmdTable = cmdTable;
    ctx->cmdTableEnd = cmdTableEnd;

    for (i = 0; i < ARRAY_COUNT(ctx->data); i++)
        ctx->data[i] = 0;

    for (i = 0; i < ARRAY_COUNT(ctx->stack); i++)
        ctx->stack[i] = NULL;
}

void SetupBytecodeScript(struct ScriptContext *ctx, const u8 *ptr)
{
    ctx->mode = SCRIPT_MODE_BYTECODE;
    ctx->scriptPtr = ptr;
}

void SetupNativeScript(struct ScriptContext *ctx, bool8 (*ptr)(void))
{
    ctx->mode = SCRIPT_MODE_NATIVE;
    ctx->nativePtr = ptr;
}

void StopScript(struct ScriptContext *ctx)
{
    ctx->mode = SCRIPT_MODE_STOPPED;
    ctx->scriptPtr = NULL;
}

bool8 RunScriptCommand(struct ScriptContext *ctx)
{
    switch (ctx->mode)
    {
    case SCRIPT_MODE_STOPPED:
        return FALSE;
    case SCRIPT_MODE_NATIVE:
        // Try to call a function in C
        // Continue to bytecode if no function or it returns TRUE
        if (ctx->nativePtr)
        {
            if (ctx->nativePtr() == TRUE)
                ctx->mode = SCRIPT_MODE_BYTECODE;
            return TRUE;
        }
        ctx->mode = SCRIPT_MODE_BYTECODE;
        // fallthrough
    case SCRIPT_MODE_BYTECODE:
        while (1)
        {
            u8 cmdCode;
            ScrCmdFunc *func;

            if (!ctx->scriptPtr)
            {
                ctx->mode = SCRIPT_MODE_STOPPED;
                return FALSE;
            }

            cmdCode = *(ctx->scriptPtr++);
            func = &ctx->cmdTable[cmdCode];

            if (func >= ctx->cmdTableEnd)
            {
                ctx->mode = SCRIPT_MODE_STOPPED;
                return FALSE;
            }

            if ((*func)(ctx) == TRUE)
                break;
        }
    }

    return TRUE;
}

static bool8 ScriptPush(struct ScriptContext *ctx, const u8 *ptr)
{
    if (ctx->stackDepth < ARRAY_COUNT(ctx->stack) - 1)
    {
        ctx->stack[ctx->stackDepth++] = ptr;
        return FALSE;
    }

    return TRUE;
}

static const u8 *ScriptPop(struct ScriptContext *ctx)
{
    if (ctx->stackDepth == 0)
        return NULL;

    return ctx->stack[--ctx->stackDepth];
}

void ScriptJump(struct ScriptContext *ctx, const u8 *ptr)
{
    ctx->scriptPtr = ptr;
}

void ScriptCall(struct ScriptContext *ctx, const u8 *ptr)
{
    ScriptPush(ctx, ctx->scriptPtr);
    ctx->scriptPtr = ptr;
}

void ScriptReturn(struct ScriptContext *ctx)
{
    ctx->scriptPtr = ScriptPop(ctx);
}

u16 ScriptReadHalfword(struct ScriptContext *ctx)
{
    u16 value = *(ctx->scriptPtr++);
    value |= *(ctx->scriptPtr++) << 8;
    return value;
}

u32 ScriptReadWord(struct ScriptContext *ctx)
{
    u32 value0 = *(ctx->scriptPtr++);
    u32 value1 = *(ctx->scriptPtr++);
    u32 value2 = *(ctx->scriptPtr++);
    u32 value3 = *(ctx->scriptPtr++);
    return (((((value3 << 8) + value2) << 8) + value1) << 8) + value0;
}

void ScriptContext2_Enable(void)
{
    sScriptContext2Enabled = TRUE;
}

void ScriptContext2_Disable(void)
{
    sScriptContext2Enabled = FALSE;
}

bool8 ScriptContext2_IsEnabled(void)
{
    return sScriptContext2Enabled;
}

bool8 ScriptContext1_IsScriptSetUp(void)
{
    if (sScriptContext1Status == SCRIPT_ON)
        return TRUE;
    else
        return FALSE;
}

void ScriptContext1_Init(void)
{
    InitScriptContext(&sScriptContext1, gScriptCmdTable, gScriptCmdTableEnd);
    sScriptContext1Status = SCRIPT_OFF;
}

bool8 ScriptContext2_RunScript(void)
{
    if (sScriptContext1Status == SCRIPT_OFF)
        return FALSE;

    if (sScriptContext1Status == SCRIPT_WAIT)
        return FALSE;

    ScriptContext2_Enable();

    if (!RunScriptCommand(&sScriptContext1))
    {
        sScriptContext1Status = SCRIPT_OFF;
        ScriptContext2_Disable();
        return FALSE;
    }

    return TRUE;
}

void ScriptContext1_SetupScript(const u8 *ptr)
{
    InitScriptContext(&sScriptContext1, gScriptCmdTable, gScriptCmdTableEnd);
    SetupBytecodeScript(&sScriptContext1, ptr);
    ScriptContext2_Enable();
    sScriptContext1Status = SCRIPT_ON;
}

void ScriptContext1_Stop(void)
{
    sScriptContext1Status = SCRIPT_WAIT;
}

void EnableBothScriptContexts(void)
{
    sScriptContext1Status = SCRIPT_ON;
    ScriptContext2_Enable();
}

void ScriptContext2_RunNewScript(const u8 *ptr)
{
    InitScriptContext(&sScriptContext2, gScriptCmdTable, gScriptCmdTableEnd);
    SetupBytecodeScript(&sScriptContext2, ptr);
    while (RunScriptCommand(&sScriptContext2));
}

static const u8 *MapHeaderGetScriptTable(u8 tag)
{
    const u8 *mapScripts = gMapHeader.mapScripts;

    if (!mapScripts)
        return NULL;

    while (1)
    {
        if (!*mapScripts)
            return NULL;
        if (*mapScripts == tag)
        {
            mapScripts++;
            return T2_READ_PTR(mapScripts);
        }
        mapScripts += 5;
    }
}

static void MapHeaderRunScriptType(u8 tag)
{
    const u8 *ptr = MapHeaderGetScriptTable(tag);
    if (ptr)
        ScriptContext2_RunNewScript(ptr);
}

static const u8 *MapHeaderCheckScriptTable(u8 tag)
{
    const u8 *ptr = MapHeaderGetScriptTable(tag);

    if (!ptr)
        return NULL;

    while (1)
    {
        u16 varIndex1;
        u16 varIndex2;

        // Read first var (or .2byte terminal value)
        varIndex1 = T1_READ_16(ptr);
        if (!varIndex1)
            return NULL; // Reached end of table
        ptr += 2;

        // Read second var
        varIndex2 = T1_READ_16(ptr);
        ptr += 2;

        // Run map script if vars are equal
        if (VarGet(varIndex1) == VarGet(varIndex2))
            return T2_READ_PTR(ptr);
        ptr += 4;
    }
}

void RunOnLoadMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_LOAD);
}

void RunOnTransitionMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_TRANSITION);
}

void RunOnResumeMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_RESUME);
}

void RunOnReturnToFieldMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_RETURN_TO_FIELD);
}

void RunOnDiveWarpMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_DIVE_WARP);
}

bool8 TryRunOnFrameMapScript(void)
{
    const u8 *ptr = MapHeaderCheckScriptTable(MAP_SCRIPT_ON_FRAME_TABLE);

    if (!ptr)
        return FALSE;

    ScriptContext1_SetupScript(ptr);
    return TRUE;
}

void TryRunOnWarpIntoMapScript(void)
{
    const u8 *ptr = MapHeaderCheckScriptTable(MAP_SCRIPT_ON_WARP_INTO_MAP_TABLE);
    if (ptr)
        ScriptContext2_RunNewScript(ptr);
}

static u32 CalculateRamScriptChecksum(void)
{
    return CalcCRC16WithTable((u8*)(&gSaveBlock1Ptr->ramScript.data), sizeof(gSaveBlock1Ptr->ramScript.data));
}

void ClearRamScript(void)
{
    CpuFill32(0, &gSaveBlock1Ptr->ramScript, sizeof(gSaveBlock1Ptr->ramScript));
}

bool32 InitRamScript(const u8 *script, u16 scriptSize, u8 mapGroup, u8 mapNum, u8 objectId)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;

    ClearRamScript();

    if (scriptSize > sizeof(scriptData->script))
        return FALSE;

    scriptData->magic = RAM_SCRIPT_MAGIC;
    scriptData->mapGroup = mapGroup;
    scriptData->mapNum = mapNum;
    scriptData->objectId = objectId;
    memcpy(scriptData->script, script, scriptSize);
    gSaveBlock1Ptr->ramScript.checksum = CalculateRamScriptChecksum();
    return TRUE;
}

const u8 *GetRamScript(u8 objectId, const u8 *script)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;
    gRamScriptRetAddr = NULL;
    if (scriptData->magic != RAM_SCRIPT_MAGIC)
        return script;
    if (scriptData->mapGroup != gSaveBlock1Ptr->location.mapGroup)
        return script;
    if (scriptData->mapNum != gSaveBlock1Ptr->location.mapNum)
        return script;
    if (scriptData->objectId != objectId)
        return script;
    if (CalculateRamScriptChecksum() != gSaveBlock1Ptr->ramScript.checksum)
    {
        ClearRamScript();
        return script;
    }

    gRamScriptRetAddr = script;
    return scriptData->script;
}

bool32 ValidateSavedRamScript(void)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;
    if (scriptData->magic != RAM_SCRIPT_MAGIC)
        return FALSE;
    if (scriptData->mapGroup != 0xFF)
        return FALSE;
    if (scriptData->mapNum != 0xFF)
        return FALSE;
    if (scriptData->objectId != 0xFF)
        return FALSE;
    if (CalculateRamScriptChecksum() != gSaveBlock1Ptr->ramScript.checksum)
        return FALSE;
    return TRUE;
}

u8 *GetSavedRamScriptIfValid(void)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;
    if (!ValidateReceivedWonderCard())
        return NULL;
    if (scriptData->magic != RAM_SCRIPT_MAGIC)
        return NULL;
    if (scriptData->mapGroup != 0xFF)
        return NULL;
    if (scriptData->mapNum != 0xFF)
        return NULL;
    if (scriptData->objectId != 0xFF)
        return NULL;
    if (CalculateRamScriptChecksum() != gSaveBlock1Ptr->ramScript.checksum)
    {
        ClearRamScript();
        return NULL;
    }
    return scriptData->script;
}

void InitRamScript_NoObjectEvent(u8 *script, u16 scriptSize)
{
    if (scriptSize > sizeof(gSaveBlock1Ptr->ramScript.data.script))
        scriptSize = sizeof(gSaveBlock1Ptr->ramScript.data.script);
    InitRamScript(script, scriptSize, 0xFF, 0xFF, 0xFF);
}
