/*
 * QEMU Geforce NV2A GPU pixel shader translation
 *
 * Copyright (c) 2013 espes
 *
 * Based on:
 * Cxbx, PixelShader.cpp
 * Copyright (c) 2004 Aaron Robinson <caustik@caustik.com>
 *                    Kingofc <kingofc@freenet.de>
 * Xeon, XBD3DPixelShader.cpp
 * Copyright (c) 2003 _SF_
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "qapi/qmp/qstring.h"

#include "hw/xbox/nv2a_gpu_psh.h"

/*
 * This implements translation of register combiners into glsl
 * fragment shaders, but all terminology is in terms of Xbox DirectX
 * pixel shaders, since I wanted to be lazy while referencing existing
 * work / stealing code.
 *
 * For some background, see the OpenGL extension:
 * https://www.opengl.org/registry/specs/NV/register_combiners.txt
 */


enum PS_TEXTUREMODES
{                                 // valid in stage 0 1 2 3
    PS_TEXTUREMODES_NONE=                 0x00L, // * * * *
    PS_TEXTUREMODES_PROJECT2D=            0x01L, // * * * *
    PS_TEXTUREMODES_PROJECT3D=            0x02L, // * * * *
    PS_TEXTUREMODES_CUBEMAP=              0x03L, // * * * *
    PS_TEXTUREMODES_PASSTHRU=             0x04L, // * * * *
    PS_TEXTUREMODES_CLIPPLANE=            0x05L, // * * * *
    PS_TEXTUREMODES_BUMPENVMAP=           0x06L, // - * * *
    PS_TEXTUREMODES_BUMPENVMAP_LUM=       0x07L, // - * * *
    PS_TEXTUREMODES_BRDF=                 0x08L, // - - * *
    PS_TEXTUREMODES_DOT_ST=               0x09L, // - - * *
    PS_TEXTUREMODES_DOT_ZW=               0x0aL, // - - * *
    PS_TEXTUREMODES_DOT_RFLCT_DIFF=       0x0bL, // - - * -
    PS_TEXTUREMODES_DOT_RFLCT_SPEC=       0x0cL, // - - - *
    PS_TEXTUREMODES_DOT_STR_3D=           0x0dL, // - - - *
    PS_TEXTUREMODES_DOT_STR_CUBE=         0x0eL, // - - - *
    PS_TEXTUREMODES_DPNDNT_AR=            0x0fL, // - * * *
    PS_TEXTUREMODES_DPNDNT_GB=            0x10L, // - * * *
    PS_TEXTUREMODES_DOTPRODUCT=           0x11L, // - * * -
    PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST= 0x12L, // - - - *
    // 0x13-0x1f reserved
};

enum PS_INPUTMAPPING
{
    PS_INPUTMAPPING_UNSIGNED_IDENTITY= 0x00L, // max(0,x)         OK for final combiner
    PS_INPUTMAPPING_UNSIGNED_INVERT=   0x20L, // 1 - max(0,x)     OK for final combiner
    PS_INPUTMAPPING_EXPAND_NORMAL=     0x40L, // 2*max(0,x) - 1   invalid for final combiner
    PS_INPUTMAPPING_EXPAND_NEGATE=     0x60L, // 1 - 2*max(0,x)   invalid for final combiner
    PS_INPUTMAPPING_HALFBIAS_NORMAL=   0x80L, // max(0,x) - 1/2   invalid for final combiner
    PS_INPUTMAPPING_HALFBIAS_NEGATE=   0xa0L, // 1/2 - max(0,x)   invalid for final combiner
    PS_INPUTMAPPING_SIGNED_IDENTITY=   0xc0L, // x                invalid for final combiner
    PS_INPUTMAPPING_SIGNED_NEGATE=     0xe0L, // -x               invalid for final combiner
};

enum PS_REGISTER
{
    PS_REGISTER_ZERO=              0x00L, // r
    PS_REGISTER_DISCARD=           0x00L, // w
    PS_REGISTER_C0=                0x01L, // r
    PS_REGISTER_C1=                0x02L, // r
    PS_REGISTER_FOG=               0x03L, // r
    PS_REGISTER_V0=                0x04L, // r/w
    PS_REGISTER_V1=                0x05L, // r/w
    PS_REGISTER_T0=                0x08L, // r/w
    PS_REGISTER_T1=                0x09L, // r/w
    PS_REGISTER_T2=                0x0aL, // r/w
    PS_REGISTER_T3=                0x0bL, // r/w
    PS_REGISTER_R0=                0x0cL, // r/w
    PS_REGISTER_R1=                0x0dL, // r/w
    PS_REGISTER_V1R0_SUM=          0x0eL, // r
    PS_REGISTER_EF_PROD=           0x0fL, // r

    PS_REGISTER_ONE=               PS_REGISTER_ZERO | PS_INPUTMAPPING_UNSIGNED_INVERT, // OK for final combiner
    PS_REGISTER_NEGATIVE_ONE=      PS_REGISTER_ZERO | PS_INPUTMAPPING_EXPAND_NORMAL,   // invalid for final combiner
    PS_REGISTER_ONE_HALF=          PS_REGISTER_ZERO | PS_INPUTMAPPING_HALFBIAS_NEGATE, // invalid for final combiner
    PS_REGISTER_NEGATIVE_ONE_HALF= PS_REGISTER_ZERO | PS_INPUTMAPPING_HALFBIAS_NORMAL, // invalid for final combiner
};

enum PS_COMBINERCOUNTFLAGS
{
    PS_COMBINERCOUNT_MUX_LSB=     0x0000L, // mux on r0.a lsb
    PS_COMBINERCOUNT_MUX_MSB=     0x0001L, // mux on r0.a msb

    PS_COMBINERCOUNT_SAME_C0=     0x0000L, // c0 same in each stage
    PS_COMBINERCOUNT_UNIQUE_C0=   0x0010L, // c0 unique in each stage

    PS_COMBINERCOUNT_SAME_C1=     0x0000L, // c1 same in each stage
    PS_COMBINERCOUNT_UNIQUE_C1=   0x0100L  // c1 unique in each stage
};

enum PS_COMBINEROUTPUT
{
    PS_COMBINEROUTPUT_IDENTITY=            0x00L, // y = x
    PS_COMBINEROUTPUT_BIAS=                0x08L, // y = x - 0.5
    PS_COMBINEROUTPUT_SHIFTLEFT_1=         0x10L, // y = x*2
    PS_COMBINEROUTPUT_SHIFTLEFT_1_BIAS=    0x18L, // y = (x - 0.5)*2
    PS_COMBINEROUTPUT_SHIFTLEFT_2=         0x20L, // y = x*4
    PS_COMBINEROUTPUT_SHIFTRIGHT_1=        0x30L, // y = x/2

    PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA=    0x80L, // RGB only

    PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA=    0x40L, // RGB only

    PS_COMBINEROUTPUT_AB_MULTIPLY=         0x00L,
    PS_COMBINEROUTPUT_AB_DOT_PRODUCT=      0x02L, // RGB only

    PS_COMBINEROUTPUT_CD_MULTIPLY=         0x00L,
    PS_COMBINEROUTPUT_CD_DOT_PRODUCT=      0x01L, // RGB only

    PS_COMBINEROUTPUT_AB_CD_SUM=           0x00L, // 3rd output is AB+CD
    PS_COMBINEROUTPUT_AB_CD_MUX=           0x04L, // 3rd output is MUX(AB,CD) based on R0.a
};

enum PS_CHANNEL
{
    PS_CHANNEL_RGB=   0x00, // used as RGB source
    PS_CHANNEL_BLUE=  0x00, // used as ALPHA source
    PS_CHANNEL_ALPHA= 0x10, // used as RGB or ALPHA source
};


enum PS_FINALCOMBINERSETTING
{
    PS_FINALCOMBINERSETTING_CLAMP_SUM=     0x80, // V1+R0 sum clamped to [0,1]

    PS_FINALCOMBINERSETTING_COMPLEMENT_V1= 0x40, // unsigned invert mapping

    PS_FINALCOMBINERSETTING_COMPLEMENT_R0= 0x20, // unsigned invert mapping
};



// Structures to describe the PS definition

struct InputInfo {
    int reg, mod, chan;
    bool invert;
};

struct InputVarInfo {
    struct InputInfo a, b, c, d;
};

struct FCInputInfo {
    struct InputInfo a, b, c, d, e, f, g;
    int c0, c1;
    //uint32_t c0_value, c1_value;
    bool c0_used, c1_used;
    bool v1r0_sum, clamp_sum, inv_v1, inv_r0, enabled;
};

struct OutputInfo {
    int ab, cd, muxsum, flags, ab_op, cd_op, muxsum_op,
        mapping, ab_alphablue, cd_alphablue;
};

struct PSStageInfo {
    struct InputVarInfo rgb_input, alpha_input;
    struct OutputInfo rgb_output, alpha_output;
    int c0, c1;
    //uint32_t c0_value, c1_value;
    bool c0_used, c1_used;
};

struct PixelShader {
    int num_stages, flags;
    struct PSStageInfo stage[8];
    struct FCInputInfo final_input;
    int tex_modes[4], input_tex[4];

    //uint32_t compare_mode, dot_mapping, input_texture;

    bool rect_tex[4];

    QString *varE, *varF;
    QString *code;
    int cur_stage;

    int num_var_refs;
    const char var_refs[32][32];
    int num_const_refs;
    const char const_refs[32][32];
};

static void add_var_ref(struct PixelShader *ps, const char *var)
{
    int i;
    for (i=0; i<ps->num_var_refs; i++) {
        if (strcmp((char*)ps->var_refs[i], var) == 0) return;
    }
    strcpy((char*)ps->var_refs[ps->num_var_refs++], var);
}

static void add_const_ref(struct PixelShader *ps, const char *var)
{
    int i;
    for (i=0; i<ps->num_const_refs; i++) {
        if (strcmp((char*)ps->const_refs[i], var) == 0) return;
    }
    strcpy((char*)ps->const_refs[ps->num_const_refs++], var);
}

// Get the code for a variable used in the program
static QString* get_var(struct PixelShader *ps, int reg, bool is_dest)
{
    switch (reg) {
    case PS_REGISTER_DISCARD:
        if (is_dest) {
            return qstring_from_str("");
        } else {
            return qstring_from_str("0.0");
        }
        break;
    case PS_REGISTER_C0:
        /* TODO: should the final stage really always be unique? */
        if (ps->flags & PS_COMBINERCOUNT_UNIQUE_C0 || ps->cur_stage == 8) {
            QString *reg = qstring_from_fmt("c_%d_%d", ps->cur_stage, 0);
            add_const_ref(ps, qstring_get_str(reg));
            if (ps->cur_stage == 8) {
                ps->final_input.c0_used = true;
            } else {
                ps->stage[ps->cur_stage].c0_used = true;
            }
            return reg;
        } else {  // Same c0
            add_const_ref(ps, "c_0_0");
            ps->stage[0].c0_used = true;
            return qstring_from_str("c_0_0");
        }
        break;
    case PS_REGISTER_C1:
        if (ps->flags & PS_COMBINERCOUNT_UNIQUE_C1 || ps->cur_stage == 8) {
            QString *reg = qstring_from_fmt("c_%d_%d", ps->cur_stage, 1);
            add_const_ref(ps, qstring_get_str(reg));
            if (ps->cur_stage == 8) {
                ps->final_input.c1_used = true;
            } else {
                ps->stage[ps->cur_stage].c1_used = true;
            }
            return reg;
        } else {  // Same c1
            add_const_ref(ps, "c_0_1");
            ps->stage[0].c1_used = true;
            return qstring_from_str("c_0_1");
        }
        break;
    case PS_REGISTER_FOG: // TODO
        //return qstring_from_str("fog");
        return qstring_from_str("vec4(1.0)");
    case PS_REGISTER_V0:
        return qstring_from_str("v0");
    case PS_REGISTER_V1:
        return qstring_from_str("v1");
    case PS_REGISTER_T0:
        return qstring_from_str("t0");
    case PS_REGISTER_T1:
        return qstring_from_str("t1");
    case PS_REGISTER_T2:
        return qstring_from_str("t2");
    case PS_REGISTER_T3:
        return qstring_from_str("t3");
    case PS_REGISTER_R0:
        add_var_ref(ps, "r0");
        return qstring_from_str("r0");
    case PS_REGISTER_R1:
        add_var_ref(ps, "r1");
        return qstring_from_str("r1");
    case PS_REGISTER_V1R0_SUM:
        add_var_ref(ps, "r0");
        return qstring_from_str("(v1 + r0)");
    case PS_REGISTER_EF_PROD:
        return qstring_from_fmt("(%s * %s)", qstring_get_str(ps->varE),
                                qstring_get_str(ps->varF));
    default:
        assert(false);
        break;
    }
}

// Get input variable code
static QString* get_input_var(struct PixelShader *ps, struct InputInfo in, bool is_alpha)
{
    QString *reg = get_var(ps, in.reg, false);

    if (strcmp(qstring_get_str(reg), "0.0") != 0
        && (in.reg != PS_REGISTER_EF_PROD
            || strstr(qstring_get_str(reg), ".a") == NULL)) {
        switch (in.chan) {
        case PS_CHANNEL_RGB:
            if (is_alpha) {
                qstring_append(reg, ".b");
            } else {
                qstring_append(reg, ".rgb");
            }
            break;
        case PS_CHANNEL_ALPHA:
            qstring_append(reg, ".a");
            break;
        default:
            assert(false);
            break;
        }
    }

    QString *res;
    switch (in.mod) {
    case PS_INPUTMAPPING_SIGNED_IDENTITY:
    case PS_INPUTMAPPING_UNSIGNED_IDENTITY:
        QINCREF(reg);
        res = reg;
        break;
    case PS_INPUTMAPPING_UNSIGNED_INVERT:
        res = qstring_from_fmt("(1.0 - %s)", qstring_get_str(reg));
        break;
    case PS_INPUTMAPPING_EXPAND_NORMAL: // TODO: Change to max(0, x)??
        res = qstring_from_fmt("(2.0 * %s - 1.0)", qstring_get_str(reg));
        break;
    case PS_INPUTMAPPING_EXPAND_NEGATE:
        res = qstring_from_fmt("(1.0 - 2.0 * %s)", qstring_get_str(reg));
        break;
    case PS_INPUTMAPPING_HALFBIAS_NORMAL:
        res = qstring_from_fmt("(%s - 0.5)", qstring_get_str(reg));
        break;
    case PS_INPUTMAPPING_HALFBIAS_NEGATE:
        res = qstring_from_fmt("(0.5 - %s)", qstring_get_str(reg));
        break;
    case PS_INPUTMAPPING_SIGNED_NEGATE:
        res = qstring_from_fmt("-%s", qstring_get_str(reg));
        break;
    default:
        assert(false);
        break;
    }

    QDECREF(reg);
    return res;
}

// Get code for the output mapping of a stage
static QString* get_output(QString *reg, int mapping)
{
    QString *res;
    switch (mapping) {
    case PS_COMBINEROUTPUT_IDENTITY:
        QINCREF(reg);
        res = reg;
        break;
    case PS_COMBINEROUTPUT_BIAS:
        res = qstring_from_fmt("(%s - 0.5)", qstring_get_str(reg));
        break;
    case PS_COMBINEROUTPUT_SHIFTLEFT_1:
        res = qstring_from_fmt("(%s * 2.0)", qstring_get_str(reg));
        break;
    case PS_COMBINEROUTPUT_SHIFTLEFT_1_BIAS:
        res = qstring_from_fmt("((%s - 0.5) * 2.0)", qstring_get_str(reg));
        break;
    case PS_COMBINEROUTPUT_SHIFTLEFT_2:
        res = qstring_from_fmt("(%s * 4.0)", qstring_get_str(reg));
        break;
    case PS_COMBINEROUTPUT_SHIFTRIGHT_1:
        res = qstring_from_fmt("(%s / 2.0)", qstring_get_str(reg));
        break;
    default:
        assert(false);
        break;
    }
    return res;
}

// Add the HLSL code for a stage
static void add_stage_code(struct PixelShader *ps,
                           struct InputVarInfo input, struct OutputInfo output,
                           const char *write_mask, bool is_alpha)
{
    QString *a = get_input_var(ps, input.a, is_alpha);
    QString *b = get_input_var(ps, input.b, is_alpha);
    QString *c = get_input_var(ps, input.c, is_alpha);
    QString *d = get_input_var(ps, input.d, is_alpha);

    const char *caster = "";
    if (strlen(write_mask) == 3) {
        caster = "vec3";
    }

    QString *ab;
    if (output.ab_op == PS_COMBINEROUTPUT_AB_DOT_PRODUCT) {
        ab = qstring_from_fmt("dot(%s, %s)",
                              qstring_get_str(a), qstring_get_str(b));
    } else {
        ab = qstring_from_fmt("(%s * %s)",
                              qstring_get_str(a), qstring_get_str(b));
    }

    QString *cd;
    if (output.cd_op == PS_COMBINEROUTPUT_CD_DOT_PRODUCT) {
        cd = qstring_from_fmt("dot(%s, %s)",
                              qstring_get_str(c), qstring_get_str(d));
    } else {
        cd = qstring_from_fmt("(%s * %s)",
                              qstring_get_str(c), qstring_get_str(d));
    }

    QString *ab_mapping = get_output(ab, output.mapping);
    QString *cd_mapping = get_output(cd, output.mapping);
    QString *ab_dest = get_var(ps, output.ab, true);
    QString *cd_dest = get_var(ps, output.cd, true);
    QString *sum_dest = get_var(ps, output.muxsum, true);

    if (qstring_get_length(ab_dest)) {
        qstring_append_fmt(ps->code, "%s.%s = %s(%s);\n",
                           qstring_get_str(ab_dest), write_mask, caster, qstring_get_str(ab_mapping));
    } else {
        QINCREF(ab_mapping);
        ab_dest = ab_mapping;
    }

    if (qstring_get_length(cd_dest)) {
        qstring_append_fmt(ps->code, "%s.%s = %s(%s);\n",
                           qstring_get_str(cd_dest), write_mask, caster, qstring_get_str(cd_mapping));
    } else {
        QINCREF(cd_mapping);
        cd_dest = cd_mapping;
    }

    if (!is_alpha && output.flags & PS_COMBINEROUTPUT_AB_BLUE_TO_ALPHA) {
        qstring_append_fmt(ps->code, "%s.a = %s.b;\n",
                           qstring_get_str(ab_dest), qstring_get_str(ab_dest));
    }
    if (!is_alpha && output.flags & PS_COMBINEROUTPUT_CD_BLUE_TO_ALPHA) {
        qstring_append_fmt(ps->code, "%s.a = %s.b;\n",
                           qstring_get_str(cd_dest), qstring_get_str(cd_dest));
    }

    QString *sum;
    if (output.muxsum_op == PS_COMBINEROUTPUT_AB_CD_SUM) {
        sum = qstring_from_fmt("(%s + %s)", qstring_get_str(ab), qstring_get_str(cd));
    } else {
        sum = qstring_from_fmt("((r0.a >= 0.5) ? %s : %s)",
                               qstring_get_str(cd), qstring_get_str(ab));
    }

    QString *sum_mapping = get_output(sum, output.mapping);
    if (qstring_get_length(sum_dest)) {
        qstring_append_fmt(ps->code, "%s.%s = %s(%s);\n",
                           qstring_get_str(sum_dest), write_mask, caster, qstring_get_str(sum_mapping));
    }

    QDECREF(a);
    QDECREF(b);
    QDECREF(c);
    QDECREF(d);
    QDECREF(ab);
    QDECREF(cd);
    QDECREF(ab_mapping);
    QDECREF(cd_mapping);
    QDECREF(ab_dest);
    QDECREF(cd_dest);
    QDECREF(sum_dest);
    QDECREF(sum);
    QDECREF(sum_mapping);
}

// Add code for the final combiner stage
static void add_final_stage_code(struct PixelShader *ps, struct FCInputInfo final)
{
    ps->varE = get_input_var(ps, final.e, false);
    ps->varF = get_input_var(ps, final.f, false);

    QString *a = get_input_var(ps, final.a, false);
    QString *b = get_input_var(ps, final.b, false);
    QString *c = get_input_var(ps, final.c, false);
    QString *d = get_input_var(ps, final.d, false);
    QString *g = get_input_var(ps, final.g, false);

    add_var_ref(ps, "r0");
    qstring_append_fmt(ps->code, "r0.rgb = vec3((%s * %s) + ((1.0 - %s) * %s) + %s);\n",
                       qstring_get_str(a), qstring_get_str(b),
                       qstring_get_str(a), qstring_get_str(c), qstring_get_str(d));
    qstring_append_fmt(ps->code, "r0.a = %s;\n", qstring_get_str(g));

    QDECREF(a);
    QDECREF(b);
    QDECREF(c);
    QDECREF(d);
    QDECREF(g);

    QDECREF(ps->varE);
    QDECREF(ps->varF);
    ps->varE = ps->varF = NULL;
}



static QString* psh_convert(struct PixelShader *ps)
{
    int i;

    QString *preflight = qstring_new();
    QString *vars = qstring_new();

    qstring_append(vars, "vec4 v0 = gl_Color;\n");
    qstring_append(vars, "vec4 v1 = gl_SecondaryColor;\n");
    qstring_append(vars, "float fog = gl_FogFragCoord;\n");

    for (i = 0; i < 4; i++) {
        if (ps->tex_modes[i] == PS_TEXTUREMODES_NONE) continue;

        const char *sampler_type;
        const char *sampler_function;

        switch (ps->tex_modes[i]) {
        case PS_TEXTUREMODES_PROJECT2D:
            if (ps->rect_tex[i]) {
                sampler_type = "sampler2DRect";
                sampler_function = "texture2DRect";
            } else {
                sampler_type = "sampler2D";
                sampler_function = "texture2D";
            }
            qstring_append_fmt(vars, "vec4 t%d = %s(texSamp%d, gl_TexCoord[%d].xy);\n",
                               i, sampler_function, i, i);
            break;
        case PS_TEXTUREMODES_PROJECT3D:
            sampler_type = "sampler3D";
            qstring_append_fmt(vars, "vec4 t%d = texture3D(texSamp%d, gl_TexCoord[%d].xyz);\n",
                               i, i, i);
            break;
        case PS_TEXTUREMODES_CUBEMAP:
            sampler_type = "samplerCube";
            qstring_append_fmt(vars, "vec4 t%d = textureCube(texSamp%d, gl_TexCoord[%d].xyz);\n",
                               i, i, i);
            break;
        case PS_TEXTUREMODES_PASSTHRU:
            qstring_append_fmt(vars, "vec4 t%d;\n", i);
            break;
        default:
            printf("%x\n", ps->tex_modes[i]);
            assert(false);
            break;
        }
        
        if (ps->tex_modes[i] != PS_TEXTUREMODES_PASSTHRU) {
            qstring_append_fmt(preflight, "uniform %s texSamp%d;\n", sampler_type, i);
        }
    }

    ps->code = qstring_new();
    for (i = 0; i < ps->num_stages; i++) {
        ps->cur_stage = i;
        qstring_append_fmt(ps->code, "// Stage %d\n", i);
        add_stage_code(ps, ps->stage[i].rgb_input, ps->stage[i].rgb_output, "rgb", false);
        add_stage_code(ps, ps->stage[i].alpha_input, ps->stage[i].alpha_output, "a", true);
    }

    if (ps->final_input.enabled) {
        ps->cur_stage = 8;
        qstring_append(ps->code, "// Final Combiner\n");
        add_final_stage_code(ps, ps->final_input);
    }

    for (i = 0; i < ps->num_var_refs; i++) {
        qstring_append_fmt(vars, "vec4 %s;\n", ps->var_refs[i]);
        if (strcmp(ps->var_refs[i], "r0") == 0) {
            if (ps->tex_modes[0] != PS_TEXTUREMODES_NONE) {
                qstring_append(vars, "r0.a = t0.a;\n");
            } else {
                qstring_append(vars, "r0.a = 1.0;\n");
            }
        }
    }
    for (i = 0; i < ps->num_const_refs; i++) {
        qstring_append_fmt(preflight, "uniform vec4 %s;\n", ps->const_refs[i]);
    }



    QString *final = qstring_new();
    qstring_append(final, qstring_get_str(preflight));
    qstring_append(final, "void main() {\n");
    qstring_append(final, qstring_get_str(vars));
    qstring_append(final, qstring_get_str(ps->code));
    qstring_append(final, "gl_FragColor = r0;\n");
    qstring_append(final, "}\n");

    QDECREF(preflight);
    QDECREF(vars);
    QDECREF(ps->code);

    return final;
}

static void parse_input(struct InputInfo *var, int value)
{
    var->reg = value & 0xF;
    var->chan = value & 0x10;
    var->mod = value & 0xE0;
}

static void parse_combiner_inputs(uint32_t value,
                                struct InputInfo *a, struct InputInfo *b,
                                struct InputInfo *c, struct InputInfo *d)
{
    parse_input(d, value & 0xFF);
    parse_input(c, (value >> 8) & 0xFF);
    parse_input(b, (value >> 16) & 0xFF);
    parse_input(a, (value >> 24) & 0xFF);
}

static void parse_combiner_output(uint32_t value, struct OutputInfo *out)
{
    out->cd = value & 0xF;
    out->ab = (value >> 4) & 0xF;
    out->muxsum = (value >> 8) & 0xF;
    int flags = value >> 12;
    out->flags = flags;
    out->cd_op = flags & 1;
    out->ab_op = flags & 2;
    out->muxsum_op = flags & 4;
    out->mapping = flags & 0x38;
    out->ab_alphablue = flags & 0x80;
    out->cd_alphablue = flags & 0x40;
}

QString *psh_translate(uint32_t combiner_control, uint32_t shader_stage_program,
                       uint32_t other_stage_input,
                       uint32_t rgb_inputs[8], uint32_t rgb_outputs[8],
                       uint32_t alpha_inputs[8], uint32_t alpha_outputs[8],
                       /*uint32_t constant_0[8], uint32_t constant_1[8],*/
                       uint32_t final_inputs_0, uint32_t final_inputs_1,
                       /*uint32_t final_constant_0, uint32_t final_constant_1,*/
                       bool rect_tex[4])
{
    int i;
    struct PixelShader ps;
    memset(&ps, 0, sizeof(ps));

    ps.num_stages = combiner_control & 0xFF;
    ps.flags = combiner_control >> 8;
    for (i = 0; i < 4; i++) {
        ps.tex_modes[i] = (shader_stage_program >> (i * 5)) & 0x1F;
        ps.rect_tex[i] = rect_tex[i];
    }

    ps.input_tex[0] = -1;
    ps.input_tex[1] = 0;
    ps.input_tex[2] = (other_stage_input >> 16) & 0xF;
    ps.input_tex[3] = (other_stage_input >> 20) & 0xF;
    for (i = 0; i < ps.num_stages; i++) {
        parse_combiner_inputs(rgb_inputs[i], &ps.stage[i].rgb_input.a,
            &ps.stage[i].rgb_input.b, &ps.stage[i].rgb_input.c, &ps.stage[i].rgb_input.d);
        parse_combiner_inputs(alpha_inputs[i], &ps.stage[i].alpha_input.a,
            &ps.stage[i].alpha_input.b, &ps.stage[i].alpha_input.c, &ps.stage[i].alpha_input.d);

        parse_combiner_output(rgb_outputs[i], &ps.stage[i].rgb_output);
        parse_combiner_output(alpha_outputs[i], &ps.stage[i].alpha_output);
        //ps.stage[i].c0 = (pDef->PSC0Mapping >> (i * 4)) & 0xF;
        //ps.stage[i].c1 = (pDef->PSC1Mapping >> (i * 4)) & 0xF;
        //ps.stage[i].c0_value = constant_0[i];
        //ps.stage[i].c1_value = constant_1[i];
    }

    struct InputInfo blank;
    ps.final_input.enabled = final_inputs_0 || final_inputs_1;
    if (ps.final_input.enabled) {
        parse_combiner_inputs(final_inputs_0, &ps.final_input.a, &ps.final_input.b,
                              &ps.final_input.c, &ps.final_input.d);
        parse_combiner_inputs(final_inputs_1, &ps.final_input.e, &ps.final_input.f,
                              &ps.final_input.g, &blank);
        int flags = final_inputs_1 & 0xFF;
        ps.final_input.clamp_sum = flags & PS_FINALCOMBINERSETTING_CLAMP_SUM;
        ps.final_input.inv_v1 = flags & PS_FINALCOMBINERSETTING_COMPLEMENT_V1;
        ps.final_input.inv_r0 = flags & PS_FINALCOMBINERSETTING_COMPLEMENT_R0;
        //ps.final_input.c0 = (pDef->PSFinalCombinerConstants >> 0) & 0xF;
        //ps.final_input.c1 = (pDef->PSFinalCombinerConstants >> 4) & 0xF;
        //ps.final_input.c0_value = final_constant_0;
        //ps.final_input.c1_value = final_constant_1;
    }



    return psh_convert(&ps);
}