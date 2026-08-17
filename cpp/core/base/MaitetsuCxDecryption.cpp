#include "MaitetsuCxDecryption.h"
#include <vector>
#include <cstdint>
#include <mutex>
#include <map>
#include <stdexcept>
#include <algorithm>

namespace {

enum CxByteCode {
    OP_NOP = 0,
    OP_RETN = 1,
    OP_MOV_EDI_ARG = 2,
    OP_PUSH_EBX = 3,
    OP_POP_EBX = 4,
    OP_PUSH_ECX = 5,
    OP_POP_ECX = 6,
    OP_MOV_EAX_EBX = 7,
    OP_MOV_EBX_EAX = 8,
    OP_MOV_ECX_EBX = 9,
    OP_MOV_EAX_CONTROL_BLOCK = 10,
    OP_MOV_EAX_EDI = 11,
    OP_MOV_EAX_INDIRECT = 12,
    OP_ADD_EAX_EBX = 13,
    OP_SUB_EAX_EBX = 14,
    OP_IMUL_EAX_EBX = 15,
    OP_AND_ECX_0F = 16,
    OP_SHR_EBX_1 = 17,
    OP_SHL_EAX_1 = 18,
    OP_SHR_EAX_CL = 19,
    OP_SHL_EAX_CL = 20,
    OP_OR_EAX_EBX = 21,
    OP_NOT_EAX = 22,
    OP_NEG_EAX = 23,
    OP_DEC_EAX = 24,
    OP_INC_EAX = 25,

    OP_IMMED = 0x100,
    OP_MOV_EAX_IMMED = 0x101,
    OP_AND_EBX_IMMED = 0x102,
    OP_AND_EAX_IMMED = 0x103,
    OP_XOR_EAX_IMMED = 0x104,
    OP_ADD_EAX_IMMED = 0x105,
    OP_SUB_EAX_IMMED = 0x106
};

class CxProgram {
public:
    static const int LENGTH_LIMIT = 0x80;

    struct Context {
        uint32_t eax = 0;
        uint32_t ebx = 0;
        uint32_t ecx = 0;
        uint32_t edi = 0;
        uint32_t stack[16]{};
        int sp = 0;

        void push(uint32_t val) {
            if (sp < 16) stack[sp++] = val;
        }
        uint32_t pop() {
            return sp > 0 ? stack[--sp] : 0;
        }
    };

    std::vector<uint32_t> code;
    int length = 0;
    uint32_t seed = 0;
    const uint32_t* control_block;
    size_t control_block_size;

    CxProgram(uint32_t s, const uint32_t* cb, size_t cb_size) : seed(s), control_block(cb), control_block_size(cb_size) {}

    uint32_t execute(uint32_t hash_val) const {
        Context ctx;
        size_t pc = 0;

        while (pc < code.size()) {
            uint32_t bytecode = code[pc++];
            uint32_t immed = 0;

            if ((bytecode & OP_IMMED) == OP_IMMED) {
                if (pc >= code.size()) break;
                immed = code[pc++];
            }

            uint32_t bc_type = bytecode;

            switch (bc_type) {
                case OP_NOP: break;
                case OP_IMMED: break;
                case OP_MOV_EDI_ARG: ctx.edi = hash_val; break;
                case OP_PUSH_EBX: ctx.push(ctx.ebx); break;
                case OP_POP_EBX: ctx.ebx = ctx.pop(); break;
                case OP_PUSH_ECX: ctx.push(ctx.ecx); break;
                case OP_POP_ECX: ctx.ecx = ctx.pop(); break;
                case OP_MOV_EBX_EAX: ctx.ebx = ctx.eax; break;
                case OP_MOV_EAX_EDI: ctx.eax = ctx.edi; break;
                case OP_MOV_ECX_EBX: ctx.ecx = ctx.ebx; break;
                case OP_MOV_EAX_EBX: ctx.eax = ctx.ebx; break;
                
                case OP_AND_ECX_0F: ctx.ecx &= 0x0f; break;
                case OP_SHR_EBX_1: ctx.ebx >>= 1; break;
                case OP_SHL_EAX_1: ctx.eax <<= 1; break;
                case OP_SHR_EAX_CL: ctx.eax >>= (ctx.ecx & 0x1F); break;
                case OP_SHL_EAX_CL: ctx.eax <<= (ctx.ecx & 0x1F); break;
                case OP_OR_EAX_EBX: ctx.eax |= ctx.ebx; break;
                case OP_NOT_EAX: ctx.eax = ~ctx.eax; break;
                case OP_NEG_EAX: ctx.eax = -ctx.eax; break;
                case OP_DEC_EAX: ctx.eax -= 1; break;
                case OP_INC_EAX: ctx.eax += 1; break;

                case OP_ADD_EAX_EBX: ctx.eax += ctx.ebx; break;
                case OP_SUB_EAX_EBX: ctx.eax -= ctx.ebx; break;
                case OP_IMUL_EAX_EBX: ctx.eax *= ctx.ebx; break;

                case OP_ADD_EAX_IMMED: ctx.eax += immed; break;
                case OP_SUB_EAX_IMMED: ctx.eax -= immed; break;
                case OP_AND_EBX_IMMED: ctx.ebx &= immed; break;
                case OP_AND_EAX_IMMED: ctx.eax &= immed; break;
                case OP_XOR_EAX_IMMED: ctx.eax ^= immed; break;
                case OP_MOV_EAX_IMMED: ctx.eax = immed; break;
                case OP_MOV_EAX_INDIRECT:
                    if (ctx.eax < control_block_size) {
                        ctx.eax = ~control_block[ctx.eax];
                    }
                    break;
                
                case OP_RETN:
                    return ctx.eax;
                
                default:
                    break;
            }
        }
        return 0;
    }

    void clear() {
        length = 0;
        code.clear();
    }

    bool emit_nop(int count) {
        if (length + count > LENGTH_LIMIT) return false;
        length += count;
        return true;
    }

    bool emit(uint32_t c, int l = 1) {
        if (length + l > LENGTH_LIMIT) return false;
        length += l;
        code.push_back(c);
        return true;
    }

    bool emit_uint32(uint32_t x) {
        if (length + 4 > LENGTH_LIMIT) return false;
        length += 4;
        code.push_back(x);
        return true;
    }

    bool emit_random() {
        return emit_uint32(get_random());
    }

    uint32_t get_random() {
        uint32_t old_seed = seed;
        seed = 1103515245 * old_seed + 12345;
        return seed ^ (old_seed << 16) ^ (old_seed >> 16);
    }
};

static const uint32_t M_MASK = 574;
static const uint32_t M_OFFSET = 153;
static const int PROLOG_ORDER[] = {1, 2, 0};
static const int ODD_BRANCH_ORDER[] = {5, 2, 4, 3, 0, 1};
static const int EVEN_BRANCH_ORDER[] = {4, 0, 7, 6, 2, 1, 5, 3};
static const uint32_t CONTROL_BLOCK[] = {977085444, 1710830921, 2371333550, 1788175866, 3271112559, 2980904129, 2980746953, 2804324550, 452101482, 750916179, 1992889086, 1265035257, 992735886, 1244576377, 2988485591, 2238327590, 1665413600, 3670995269, 2490680597, 365472774, 3076700921, 1540971865, 4271432789, 1648558373, 2027861495, 2659910389, 303984789, 2441762008, 415985355, 2480128411, 2486438557, 1248332979, 2585031620, 1034381522, 2846719600, 138164541, 706132494, 4055215993, 536890713, 1868036431, 2092128072, 2062022633, 1461755994, 1387297855, 1731004215, 3484679333, 350156758, 3041517927, 1892809966, 651781517, 1144766356, 2536209355, 789886068, 3078180659, 4182633338, 4129425943, 3173590519, 4116228060, 3182344701, 2992836965, 344237597, 657780851, 2214276810, 838516216, 138533874, 3023416380, 2248872765, 1423493598, 3096443399, 1533383475, 4163004852, 1831399186, 643797739, 204959132, 3774551633, 2284703078, 2483450139, 3329061182, 363062423, 832461974, 2304073986, 618134202, 3568517724, 3766319414, 2823987763, 3842206487, 3879046767, 2025756620, 2005269650, 1786273331, 2120458041, 1001055879, 1488752335, 626463036, 2159353752, 4224361951, 2308373430, 2298387905, 2366335818, 877503140, 1584991173, 1580552423, 3613084791, 319799975, 746174230, 2011237965, 2467056682, 1576140465, 615304427, 748402174, 3292934902, 3160169945, 1369420219, 3350640022, 3811407656, 3516934800, 3577114866, 541880205, 2800756050, 3376365279, 208325402, 1135420073, 3447505442, 2621787590, 632556906, 2705781515, 1038411840, 3404375390, 3925730815, 3486762452, 1015840392, 1575730208, 2602564852, 1663900101, 198547517, 2315681569, 3568382292, 268261940, 2316999706, 1447922294, 3465970915, 508796261, 2749836569, 491819329, 795074671, 3865919416, 178559529, 4234178195, 1099521490, 525800278, 2827158737, 3342343268, 1991137001, 655979449, 2916136840, 966649429, 415084029, 3584688790, 2454592059, 1330811784, 3947160648, 4070364152, 3497484397, 1232715496, 3329414761, 314073495, 4209321214, 112650502, 1640942634, 2113325294, 3626399652, 2769020065, 3043492289, 3881421554, 578210285, 2126168151, 647340817, 2323436185, 447383016, 4198806546, 267401056, 2642402650, 2043336456, 2488337116, 441916536, 1691293179, 1918221777, 969891434, 3707860053, 2937164853, 1198708196, 4088669928, 2144977182, 3201420231, 588350541, 501496477, 3407860586, 532116772, 3606400635, 2362790756, 2833567976, 2916957880, 1331560768, 3017185389, 270730611, 699530199, 2115513617, 2729841767, 2296265548, 3045542324, 1660786500, 1764823854, 3545190258, 1918998951, 825206309, 2496518193, 344803727, 153887834, 3191714992, 1094222908, 1309173218, 2725339372, 2675971285, 309998283, 2491949185, 1123513815, 571421895, 2903137811, 2147533179, 810873021, 2386333874, 3956847966, 321950356, 3754875800, 636265610, 3907166188, 3436055468, 941064676, 2630698999, 1792969468, 305954863, 78356807, 3574862345, 2550785915, 1573387448, 3701796669, 4239202941, 3244301023, 3526114320, 809617265, 2107624205, 2122354059, 1762568978, 922609933, 1739383832, 4020186654, 920452681, 2292081220, 35646065, 2484127842, 2095234287, 1241436172, 2334378509, 3550964315, 4100267128, 3513687335, 1513791658, 1394999985, 516365803, 3186190811, 1454779937, 3066269968, 3210713171, 2122204558, 2297305412, 1976550980, 1172535354, 3081815630, 1957108879, 2170948710, 4191595493, 961340138, 4102855165, 3037770160, 772196590, 3586865417, 3838666181, 3881733872, 532220279, 3945092638, 2677764486, 2230254041, 187270937, 2268431570, 105249262, 2456315570, 1017209159, 2316621711, 2645407580, 1335511799, 1834962013, 1700194639, 4032473905, 3136240239, 1108610270, 1117247448, 2749308278, 1078863649, 878372928, 1095647985, 19639504, 790400081, 3211530967, 1154351164, 478722, 3294851926, 3401247915, 725425276, 6203756, 3972742202, 2001304062, 338468085, 1112388750, 2142643754, 1853075798, 78995279, 1823591984, 4230091376, 4161051186, 2716673026, 1349876608, 122440050, 3369563833, 3027528786, 44865017, 2641046723, 1297205277, 953045396, 2179815088, 2775460917, 2123870028, 4184116833, 4043666632, 2541722281, 3145095483, 784013995, 2820450545, 3475489155, 244922877, 4159397224, 3209459607, 4117273729, 100140495, 3682584640, 170872752, 3582224422, 2964471187, 938190389, 1285871888, 2824957935, 679183934, 2168929995, 2152501479, 1696247052, 1601905587, 49742453, 4113510899, 3630006397, 238830778, 3702766644, 3191690864, 125621375, 66968051, 597176966, 1763930461, 2058093313, 3843242166, 1695952637, 469455920, 409229803, 208661108, 3719871624, 618814695, 854419072, 3513134173, 3349233021, 1168966662, 1176718985, 2084104269, 4234264982, 2535735605, 2247504132, 3743808477, 3565649779, 679946364, 225267543, 232290371, 1314871579, 2608315286, 2159660788, 128723974, 1511244906, 1458716688, 893590556, 824592714, 2141976326, 145746040, 2867803908, 2306696209, 1366627482, 3595316637, 4187324888, 128305494, 4202298517, 2373211899, 2768555234, 3290903677, 3991176143, 1485209521, 4017739768, 658310109, 1991935213, 655248235, 1477306296, 472299580, 3910277108, 4180222895, 462648533, 3817177879, 846540300, 2107225668, 283112106, 1235430006, 1045719918, 1073224186, 679112963, 3115598978, 362767495, 934232567, 2723126861, 1212033248, 2519961408, 2595041287, 209262606, 2645896012, 250031281, 67510434, 3612497366, 1325699810, 1441797163, 2207365041, 193282913, 375169456, 3535692568, 511912024, 2904215554, 2000035576, 2119579881, 2426810528, 3930033737, 180663132, 1298546329, 1559382245, 2302264236, 843685114, 4261951026, 776791520, 2752216108, 73156213, 2355311479, 1533336483, 632960130, 758572725, 3559413356, 1396014926, 428261050, 4210161008, 721771405, 2504853826, 1903100588, 3786322845, 327283310, 1344953708, 1304560249, 3221409007, 3322781552, 3641465694, 279762509, 684627824, 3518355162, 971883180, 3486752258, 987354964, 3639908987, 126127228, 3125668839, 1584222038, 2610627517, 3060012888, 1783901167, 2734426648, 3315632228, 1663590363, 959250797, 1572973397, 2842546214, 1314315345, 64833073, 3471103628, 3674057331, 392862266, 1041600292, 2361782638, 1746049589, 80794772, 1242196480, 3940256940, 4078946917, 1510110036, 1423733842, 1119122316, 2579983177, 3102323765, 1671811421, 2921254620, 160940805, 550970584, 314288126, 3153980939, 4265168505, 4240260842, 1405077533, 1008939005, 744092321, 3258061203, 279415359, 986996191, 1751773394, 1663977551, 63022414, 1011741940, 1639130471, 3763969973, 1305554377, 3952934915, 2695701724, 1280041376, 3953313017, 3607104466, 3947079686, 1241671698, 1013643349, 1595930042, 2035731933, 2448860759, 1606727620, 694379273, 2233516251, 3743140907, 3286892645, 1891006945, 2183817763, 366387304, 1648529959, 797272824, 90841360, 4079932187, 1016933730, 2307540285, 23070344, 2771310653, 1236976951, 1253980610, 3071565001, 4119885216, 3733505588, 2837318206, 3118697397, 4269362847, 4044403356, 263270909, 438408164, 358396359, 2281812249, 2363214203, 2844873377, 1948973401, 3238227989, 2679671125, 1270127739, 475850999, 841729038, 251920878, 4176145384, 1932461169, 2110269687, 868847645, 3887326978, 1701734353, 1158281509, 2561825592, 1197660084, 414394410, 2054965694, 2579133536, 2886153999, 3246558170, 783028024, 385217230, 4087744979, 1869101237, 2376250130, 3132188197, 1310946484, 3727077791, 3803880794, 3373674670, 3186137179, 2300606165, 3112833116, 2230775106, 2648381764, 2778968400, 2302283350, 2860458105, 3234367349, 1393213864, 269771224, 1160893325, 3813353865, 2458690905, 1251146672, 3698876001, 2932085899, 51583174, 831188105, 474187965, 4184595812, 355502132, 236460902, 574863700, 3609553564, 3991161582, 1066180379, 2789170896, 3508193793, 1427894648, 2165111727, 1506213700, 4255185847, 68914621, 1812333843, 3300320797, 189224995, 3552293359, 3489965988, 1467292468, 982525286, 1527297896, 1467456124, 2825909111, 1496142440, 1968188116, 3089992912, 2276540846, 906723356, 345078352, 1742323705, 1661758409, 1091900306, 565253771, 3492066336, 1917367075, 300751632, 3864403524, 4248942808, 1042669242, 3096153762, 66849793, 1923220349, 1266346544, 2210471156, 542445431, 1483560458, 51719212, 3378282128, 3552335290, 1133358953, 3548916172, 3525558912, 3835558302, 974128220, 4179231367, 2940222570, 52905826, 3792157964, 4024668427, 2447477918, 1529654957, 2525766938, 2105429999, 4165669074, 884397826, 608666272, 525347067, 3634321094, 1424456818, 2400429834, 3486732294, 2693333853, 3634680771, 546828225, 946320864, 3579324340, 4050656012, 1213912289, 3843723430, 1686239511, 36253402, 567597019, 3035190856, 3590246929, 3808262924, 2305292368, 3395254269, 2223091315, 1769818334, 1510074046, 2830204740, 970479514, 103719617, 1138025505, 1186219098, 3746488218, 2634768524, 4084280427, 3628773583, 1983371924, 2933326804, 1967741565, 89910517, 429931826, 4044030092, 3305364700, 1113347060, 449208326, 1595372264, 290771579, 3376128975, 1711706614, 774317574, 144431143, 357798830, 1895953342, 3173989926, 2571752085, 155081318, 2757736309, 1275312198, 1499333790, 681862905, 2722506560, 3971035944, 2607887776, 2924407021, 10354779, 184350988, 207463966, 1171592939, 2473996402, 3087550168, 2875293467, 1164535169, 206441915, 798966062, 1917527142, 1839644420, 1137242144, 1248487237, 2699663783, 3510585497, 3153178245, 1868681423, 2227449965, 1005443842, 1843443245, 1288272164, 609699569, 307592339, 3701891453, 2898877572, 1564294055, 4110453163, 1862390741, 4288671630, 4089311434, 3898236064, 94268515, 323619652, 1497965043, 3657734470, 1723338640, 1494238970, 2276210536, 2978033901, 3916643788, 939284221, 2728800351, 860987527, 2933790255, 460876075, 2384946132, 4033363018, 1140827724, 512886882, 889548184, 3232285515, 787911850, 1297043833, 4202200965, 3546760332, 1618312164, 2625562763, 1615579094, 1769594212, 3021923103, 4285206956, 2040318669, 2544834552, 2472391235, 1223466360, 4158056167, 3722789030, 3159572759, 4086751408, 614932163, 3432730605, 49904459, 2919642806, 666005533, 826353314, 3617667364, 1646528722, 147312873, 742325931, 4228854382, 3128074710, 271745399, 3235376301, 3816468582, 3500739080, 588169369, 837166574, 329195849, 1733106834, 2135365788, 1279562651, 3660660025, 1094601203, 1330833770, 3760800592, 2828583430, 1053277094, 123681346, 1828261637, 2726865257, 460042195, 2824125510, 642218112, 91310985, 2279353085, 145401261, 3440010654, 89792832, 3515587796, 2753595823, 2458547616, 3794141186, 1442045028, 1272381865, 2789964778, 554332027, 3997060915, 751351878, 3521983711, 2846467072, 222591129, 2855740901, 1124090449, 2267336880, 2261078694, 278485924, 2475603604, 1926603131, 1998614448, 3594160935, 1266433655, 3629273036, 368100895, 2818875546, 1227464784, 1291269193, 3414586201, 2400423649, 2440764294, 2068869796, 130010991, 511047316, 3943718330, 3170223859, 3930123001, 2707732346, 3046559573, 1685812534, 1749616236, 2346025547, 1005818327, 732505413, 469207323, 2705300287, 3354747167, 1098455406, 896765095, 292868583, 4113568859, 67215730, 3443172046, 1105146588, 978803629, 3886087899, 743283152, 2173864401, 3804313173, 1426153148, 2955446835, 377297079, 3512759610, 2675482645, 4068671143, 4052992995, 4167806134, 1767695711, 3693564140, 4189554733, 532963987, 3915638131, 4283930005, 4253417185, 552862983, 152130934, 2158641901, 4243382716, 2375469559, 1718815896, 228590525, 2107169548, 1667012616, 3306593273, 2227657673, 2837829299, 183818230, 174682261, 2035910359, 3862347503, 2332919128, 789802411, 1462232754, 81021752, 1638277337, 383577388, 1529901473, 3095008379, 2415723318, 1406318542, 4189423866, 495292065, 428765069, 506368401, 2599003715, 67247104, 3636272568, 315316466, 3220102234, 353777824, 641989596, 357615334, 2528110674, 208370168, 913646112, 3014925483, 1265160664, 1154311794, 4225861835, 511470895, 3729187943, 1206602167, 1498347779, 1066231442, 3905016500, 3890107946, 4208212908, 2483720264, 1272896702, 1708678519, 4150537683, 2898908239, 3770960231, 1986519596, 522431305, 1858099913, 4257466373, 3592682300, 273247969, 227122384, 1342645518, 196612306, 2054817079, 3252235520, 4163094227, 2168067901, 1124262542, 2616899381, 1287316131, 2461641833};

class MaitetsuCxEncryption {
public:
    static MaitetsuCxEncryption& getInstance() {
        static MaitetsuCxEncryption instance;
        return instance;
    }

    void decrypt_buffer(uint32_t hash_val, uint64_t offset, uint8_t* buffer, size_t count) {
        uint32_t key = hash_val;
        uint32_t base_offset = get_base_offset(key);
        
        if (offset < base_offset) {
            uint64_t base_length = std::min((uint64_t)(base_offset - offset), (uint64_t)count);
            
            decode(key, offset, buffer, (size_t)base_length);
            offset += base_length;
            buffer += base_length;
            count -= base_length;
        }
            
        if (count > 0) {
            key = ((key >> 16) ^ key) & 0xFFFFFFFF;
            decode(key, offset, buffer, count);
        }
    }

private:
    MaitetsuCxEncryption() {
        for (int i = 0; i < 128; i++) {
            m_program_list[i] = nullptr;
        }
    }
    ~MaitetsuCxEncryption() {
        for (int i = 0; i < 128; i++) {
            if (m_program_list[i]) delete m_program_list[i];
        }
    }

    CxProgram* m_program_list[128];
    std::mutex m_mutex;

    uint32_t get_base_offset(uint32_t hash_val) const {
        return ((hash_val & M_MASK) + M_OFFSET) & 0xFFFFFFFF;
    }

    void decode(uint32_t key, uint64_t offset, uint8_t* buffer, size_t count) {
        std::pair<uint32_t, uint32_t> ret = execute_xcode(key);
        uint32_t ret1 = ret.first;
        uint32_t ret2 = ret.second;
        
        uint32_t key1 = ret2 >> 16;
        uint32_t key2 = ret2 & 0xFFFF;
        uint32_t key3 = ret1 & 0xFF;
        
        if (key1 == key2) {
            key2 = (key2 + 1) & 0xFFFF;
        }
        if (key3 == 0) {
            key3 = 1;
        }
            
        if (key2 >= offset && key2 < offset + count) {
            size_t idx = (size_t)(key2 - offset);
            buffer[idx] ^= (ret1 >> 16) & 0xFF;
        }

        if (key1 >= offset && key1 < offset + count) {
            size_t idx = (size_t)(key1 - offset);
            buffer[idx] ^= (ret1 >> 8) & 0xFF;
        }
            
        for (size_t i = 0; i < count; i++) {
            buffer[i] ^= key3;
        }
    }

    std::pair<uint32_t, uint32_t> execute_xcode(uint32_t hash_val) {
        uint32_t seed = hash_val & 0x7f;
        CxProgram* program = nullptr;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_program_list[seed]) {
                m_program_list[seed] = generate_program(seed);
            }
            program = m_program_list[seed];
        }
        
        uint32_t hash_shifted = hash_val >> 7;
        uint32_t ret1 = program->execute(hash_shifted);
        uint32_t ret2 = program->execute((~hash_shifted) & 0xFFFFFFFF);
        
        return {ret1, ret2};
    }

    CxProgram* generate_program(uint32_t seed) {
        CxProgram* program = new CxProgram(seed, CONTROL_BLOCK, sizeof(CONTROL_BLOCK) / sizeof(CONTROL_BLOCK[0]));
        for (int stage = 5; stage >= 1; stage--) {
            if (emit_code(program, stage)) {
                return program;
            }
            program->clear();
        }
        delete program;
        throw std::runtime_error("Overly large CxEncryption bytecode");
    }

    bool emit_code(CxProgram* program, int stage) {
        return program->emit_nop(5)
            && program->emit(OP_MOV_EDI_ARG, 4)
            && emit_body(program, stage)
            && program->emit_nop(5)
            && program->emit(OP_RETN);
    }

    bool emit_body(CxProgram* program, int stage) {
        if (stage == 1) {
            return emit_prolog(program);
        }
            
        if (!program->emit(OP_PUSH_EBX)) return false;
            
        if ((program->get_random() & 1) != 0) {
            if (!emit_body(program, stage - 1)) return false;
        } else {
            if (!emit_body2(program, stage - 1)) return false;
        }
            
        if (!program->emit(OP_MOV_EBX_EAX, 2)) return false;
            
        if ((program->get_random() & 1) != 0) {
            if (!emit_body(program, stage - 1)) return false;
        } else {
            if (!emit_body2(program, stage - 1)) return false;
        }
            
        return emit_odd_branch(program) && program->emit(OP_POP_EBX);
    }

    bool emit_body2(CxProgram* program, int stage) {
        if (stage == 1) {
            return emit_prolog(program);
        }
            
        bool rc = true;
        if ((program->get_random() & 1) != 0) {
            rc = emit_body(program, stage - 1);
        } else {
            rc = emit_body2(program, stage - 1);
        }
            
        return rc && emit_even_branch(program);
    }

    bool emit_prolog(CxProgram* program) {
        bool rc = true;
        int choice = PROLOG_ORDER[program->get_random() % 3];
        
        if (choice == 2) {
            rc = program->emit_nop(5)
              && program->emit(OP_MOV_EAX_IMMED, 2)
              && program->emit_uint32(program->get_random() & 0x3ff)
              && program->emit(OP_MOV_EAX_INDIRECT, 0);
        } else if (choice == 1) {
             rc = program->emit(OP_MOV_EAX_EDI, 2);
        } else if (choice == 0) {
             rc = program->emit(OP_MOV_EAX_IMMED) && program->emit_random();
        }
        return rc;
    }

    bool emit_even_branch(CxProgram* program) {
        bool rc = true;
        int choice = EVEN_BRANCH_ORDER[program->get_random() & 7];
        
        if (choice == 0) {
            rc = program->emit(OP_NOT_EAX, 2);
        } else if (choice == 1) {
            rc = program->emit(OP_DEC_EAX);
        } else if (choice == 2) {
            rc = program->emit(OP_NEG_EAX, 2);
        } else if (choice == 3) {
            rc = program->emit(OP_INC_EAX);
        } else if (choice == 4) {
            rc = program->emit_nop(5)
              && program->emit(OP_AND_EAX_IMMED)
              && program->emit_uint32(0x3ff)
              && program->emit(OP_MOV_EAX_INDIRECT, 3);
        } else if (choice == 5) {
            rc = program->emit(OP_PUSH_EBX)
              && program->emit(OP_MOV_EBX_EAX, 2)
              && program->emit(OP_AND_EBX_IMMED, 2)
              && program->emit_uint32(0xaaaaaaaa)
              && program->emit(OP_AND_EAX_IMMED)
              && program->emit_uint32(0x55555555)
              && program->emit(OP_SHR_EBX_1, 2)
              && program->emit(OP_SHL_EAX_1, 2)
              && program->emit(OP_OR_EAX_EBX, 2)
              && program->emit(OP_POP_EBX);
        } else if (choice == 6) {
            rc = program->emit(OP_XOR_EAX_IMMED) && program->emit_random();
        } else if (choice == 7) {
            if ((program->get_random() & 1) != 0) {
                rc = program->emit(OP_ADD_EAX_IMMED);
            } else {
                rc = program->emit(OP_SUB_EAX_IMMED);
            }
            rc = rc && program->emit_random();
        }
        return rc;
    }

    bool emit_odd_branch(CxProgram* program) {
        bool rc = true;
        int choice = ODD_BRANCH_ORDER[program->get_random() % 6];
        
        if (choice == 0) {
            rc = program->emit(OP_PUSH_ECX)
              && program->emit(OP_MOV_ECX_EBX, 2)
              && program->emit(OP_AND_ECX_0F, 3)
              && program->emit(OP_SHR_EAX_CL, 2)
              && program->emit(OP_POP_ECX);
        } else if (choice == 1) {
            rc = program->emit(OP_PUSH_ECX)
              && program->emit(OP_MOV_ECX_EBX, 2)
              && program->emit(OP_AND_ECX_0F, 3)
              && program->emit(OP_SHL_EAX_CL, 2)
              && program->emit(OP_POP_ECX);
        } else if (choice == 2) {
            rc = program->emit(OP_ADD_EAX_EBX, 2);
        } else if (choice == 3) {
            rc = program->emit(OP_NEG_EAX, 2)
              && program->emit(OP_ADD_EAX_EBX, 2);
        } else if (choice == 4) {
            rc = program->emit(OP_IMUL_EAX_EBX, 3);
        } else if (choice == 5) {
            rc = program->emit(OP_SUB_EAX_EBX, 2);
        }
        return rc;
    }
};

} // namespace

static std::map<uint32_t, bool> s_is_encrypted_map;
static std::mutex s_map_mutex;

void MaitetsuCxExtractionFilter(tTVPXP3ExtractionFilterInfo *info) {
    if (!info || !info->Buffer || info->BufferSize == 0) return;

    uint32_t hash_val = info->FileHash;
    if (hash_val == 0) return;

    bool should_decrypt = true;
    
    if (info->Offset == 0) {
        // Auto-detect if file is ALREADY unencrypted
        uint8_t *buf = static_cast<uint8_t*>(info->Buffer);
        bool is_unencrypted = false;
        if (info->BufferSize >= 4) {
            if (buf[0] == 'T' && buf[1] == 'J' && buf[2] == 'S' && buf[3] == '2') is_unencrypted = true;
            else if (buf[0] == '/' && buf[1] == '/') is_unencrypted = true;
            else if (buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G') is_unencrypted = true;
            else if (buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F') is_unencrypted = true;
            else if (buf[0] == 'O' && buf[1] == 'g' && buf[2] == 'g' && buf[3] == 'S') is_unencrypted = true;
            else if (buf[0] == 'P' && buf[1] == 'S' && buf[2] == 'B' && buf[3] == 0x00) is_unencrypted = true;
            else if (buf[0] == 'B' && buf[1] == 'M') is_unencrypted = true;
            else if (buf[0] == 0xFF && buf[1] == 0xFE) is_unencrypted = true; // UTF-16 LE
            else if (buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) is_unencrypted = true; // UTF-8
        }
        
        std::lock_guard<std::mutex> lock(s_map_mutex);
        s_is_encrypted_map[hash_val] = !is_unencrypted;
        should_decrypt = !is_unencrypted;
    } else {
        std::lock_guard<std::mutex> lock(s_map_mutex);
        auto it = s_is_encrypted_map.find(hash_val);
        if (it != s_is_encrypted_map.end()) {
            should_decrypt = it->second;
        }
    }

    if (!should_decrypt) return;

    uint8_t *buf = static_cast<uint8_t*>(info->Buffer);
    uint64_t pos = info->Offset;

    MaitetsuCxEncryption::getInstance().decrypt_buffer(hash_val, pos, buf, info->BufferSize);
}
