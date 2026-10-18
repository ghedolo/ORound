// Wave equation row update with ESP32-S3 PIE vector instructions (8 x int16 per block).
//
// void pie_wave_row(int16_t *prev, const int16_t *cur, int blocks, const int16_t *coef, int stride);
//   a2 prev   : row of the previous field at the first cell; overwritten with the new field
//   a3 cur    : same cell in the current field
//   a4 blocks : number of 8-cell blocks
//   a5 coef   : 3 vectors of 8 x int16 (Q15): center, previous, neighbor
//   a6 stride : row stride in bytes
// All pointers and the stride are multiples of 16 bytes.
//
// new = (cc * c + cp * p + cs * (left + right + up + down)) >> 15, then +1 where the result
// is negative, so the rounding goes toward zero (symmetric, small waves still decay).

    .text
    .align  4
    .global pie_wave_row
    .type   pie_wave_row, @function

pie_wave_row:
    entry   a1, 32
    ee.vld.128.ip   q0, a5, 16          // cc
    ee.vld.128.ip   q1, a5, 16          // cp
    ee.vld.128.ip   q2, a5, 16          // cs
    movi    a8, 15                      // Q15 shift
    beqz    a4, .Ldone

    loopnez a4, .Lloop_end
    // Left neighbors: aligned block before and current block, shifted by 14 bytes.
    // The unaligned address sets SAR_BYTE = 14 and loads the block below it.
    addi    a12, a3, -2
    ee.ld.128.usar.ip   q4, a12, 0      // block x-8..x-1, SAR_BYTE = 14
    ee.vld.128.ip   q3, a3, 0           // block x..x+7
    ee.src.q        q6, q4, q3          // x-1..x+6
    // Right neighbors: current block and next block, shifted by 2 bytes
    addi    a12, a3, 2
    ee.ld.128.usar.ip   q4, a12, 0      // block x..x+7, SAR_BYTE = 2
    addi    a13, a3, 16
    ee.vld.128.ip   q5, a13, 0          // block x+8..x+15
    ee.src.q        q4, q4, q5          // x+1..x+8

    ee.zero.qacc
    ee.vmulas.s16.qacc  q6, q2          // left
    ee.vmulas.s16.qacc  q4, q2          // right
    ee.vmulas.s16.qacc  q3, q0          // center
    sub     a12, a3, a6
    ee.vld.128.ip   q3, a12, 0          // up
    ee.vmulas.s16.qacc  q3, q2
    add     a12, a3, a6
    ee.vld.128.ip   q3, a12, 0          // down
    ee.vmulas.s16.qacc  q3, q2
    ee.vld.128.ip   q3, a2, 0           // previous
    ee.vmulas.s16.qacc  q3, q1

    ee.srcmb.s16.qacc   q4, a8, 0       // >> 15 with saturation (rounds toward -inf)
    ee.zero.q       q5
    ee.vcmp.lt.s16  q6, q4, q5          // -1 where negative
    ee.vsubs.s16    q4, q4, q6          // +1 where negative
    ee.vst.128.ip   q4, a2, 16
    addi    a3, a3, 16
.Lloop_end:

.Ldone:
    retw.n
