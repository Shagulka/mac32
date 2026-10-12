// i386 code compiled by clang, to run under the probe: a loop with memory traffic
unsigned crc32_i386(const unsigned char *p, unsigned n) {
	unsigned c = 0xffffffff;
	for (unsigned i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320 & -(c & 1));
	}
	return ~c;
}
