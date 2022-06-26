#undef explicit_bzero
void explicit_bzero(void*, size_t);

int min(int a, int b) {
    return a < b ? a : b;
}
