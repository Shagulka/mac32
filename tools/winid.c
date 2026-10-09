// Print the CGWindowID of the largest on-screen window owned by a process.
//   winid <pid>
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
	if (argc < 2) return 2;
	int pid = atoi(argv[1]);
	CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
	long best = 0;
	double area = 0;
	for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
		CFDictionaryRef w = CFArrayGetValueAtIndex(list, i);
		int owner = 0;
		long id = 0;
		CFNumberGetValue(CFDictionaryGetValue(w, kCGWindowOwnerPID), kCFNumberIntType, &owner);
		CFNumberGetValue(CFDictionaryGetValue(w, kCGWindowNumber), kCFNumberLongType, &id);
		CGRect b;
		CGRectMakeWithDictionaryRepresentation(CFDictionaryGetValue(w, kCGWindowBounds), &b);
		if (owner == pid && b.size.width * b.size.height > area) { area = b.size.width * b.size.height; best = id; }
	}
	if (!best) return 1;
	printf("%ld\n", best);
	return 0;
}
