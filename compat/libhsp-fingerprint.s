# QByteArray fingerprint(const HspImageHolder *img)   (System V: rdi = sret, rsi = holder)
# Replaces MD5(QImage::bits(), QImage::sizeInBytes()) in libhsp.so with a fast
# 64-bit hash: h = n; for each 8-byte word w: h = rotl((h ^ w) * K, 31);
# a trailing 4-byte word is mixed in the same way. Every step is a bijection of
# h, so a change in any single word always changes the result.
        .text
        .globl fp
fp:
        push    %rbp
        push    %rbx
        sub     $24, %rsp
        mov     %rdi, %rbp              # sret QByteArray*
        lea     0x18(%rsi), %rbx        # QImage*
        mov     %rbx, %rdi
        call    QImage_bits             # const uchar *QImage::bits() const
        mov     %rax, (%rsp)
        mov     %rbx, %rdi
        call    QImage_sizeInBytes      # qsizetype QImage::sizeInBytes() const
        mov     (%rsp), %rsi            # p
        mov     %rax, %rdx              # h = n ; rax = bytes left
1:      sub     $8, %rax
        jb      2f
        xor     (%rsi), %rdx
        imul    $0x7f4a7c15, %rdx, %rdx
        rol     $31, %rdx
        add     $8, %rsi
        jmp     1b
2:      test    $4, %al                 # (left - 8) & 4 == left & 4
        jz      3f
        mov     (%rsi), %ecx
        xor     %rcx, %rdx
        imul    $0x7f4a7c15, %rdx, %rdx
3:      mov     %rdx, (%rsp)
        mov     %rsp, %rsi
        mov     %rbp, %rdi
        push    $8
        pop     %rdx
        call    QByteArray_ctor         # QByteArray(const char *, int size = 8)
        mov     %rbp, %rax
        add     $24, %rsp
        pop     %rbx
        pop     %rbp
        ret
