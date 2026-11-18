#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>
extern int hal_main(int,char**);
int main(int argc,char**argv){@autoreleasepool{[NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{exit(hal_main(argc,argv));});[NSApp run];}return 1;}
