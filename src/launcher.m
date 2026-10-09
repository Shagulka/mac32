// Mac32.app opened from Finder with no program: take one from "Open With",
// a drop on the icon, or a file panel, then hand it to `mac32 run` (the
// command-line front end inside the bundle), which sets up and logs the run.
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <unistd.h>
#include <mach-o/dyld.h>

@interface M32Picker : NSObject <NSApplicationDelegate>
@property (copy) NSString *picked;
@end
@implementation M32Picker
- (void)finish
{
	[NSApp stop:nil];
	NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
				       timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
	[NSApp postEvent:e atStart:YES];	// wake the run loop so -stop: takes effect
}
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls
{
	(void)app;
	if (!self.picked && urls.count) { self.picked = urls[0].path; [self finish]; }
}
- (void)applicationDidFinishLaunching:(NSNotification *)n
{
	(void)n;
	// an "open" event comes right after launch when Finder started us with a file
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 400 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
		if (self.picked) return;
		[NSApp activateIgnoringOtherApps:YES];
		NSOpenPanel *p = [NSOpenPanel openPanel];
		p.title = @"Run a 32-bit Mac app with mac32";
		p.message = @"Choose an old 32-bit Intel app (or its program file).";
		p.prompt = @"Run";
		p.canChooseFiles = YES;
		p.canChooseDirectories = NO;
		p.treatsFilePackagesAsDirectories = NO;
		p.allowedContentTypes = @[ UTTypeApplicationBundle, UTTypeUnixExecutable, UTTypeExecutable ];
		if ([p runModal] == NSModalResponseOK) self.picked = p.URL.path;
		[self finish];
	});
}
@end

// returns only if nothing was picked
void launcher_main(void)
{
	char self[PATH_MAX];
	uint32_t sz = sizeof self;
	if (_NSGetExecutablePath(self, &sz)) return;
	NSString *path;
	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		M32Picker *d = [M32Picker new];
		NSApp.delegate = d;
		[NSApp run];
		path = [d.picked copy];
	}
	if (!path) return;
	// Contents/MacOS/mac32 -> Contents/Resources/mac32 (the front end)
	NSString *cli = [[[[NSString stringWithUTF8String:self] stringByDeletingLastPathComponent]
			  stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"Resources/mac32"];
	const char *argv[] = { "/bin/bash", cli.fileSystemRepresentation, "run", path.fileSystemRepresentation, NULL };
	execv("/bin/bash", (char *const *)argv);
	fprintf(stderr, "mac32: cannot start %s\n", cli.UTF8String);
}
