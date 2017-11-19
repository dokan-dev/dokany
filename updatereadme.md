Windows is extremely technical. By using Dokan, you can create
 your own file systems very easily without writing device drivers. 
 Dokan is similar to FUSE (Linux file system in user space) but works on Windows.
-Moreover dokany include a [FUSE wrapper](https://github.com/dokan-dev/dokany/wiki/FUSE) that help you to port your FUSE 
filesystems without changes
+Moreover dokany include a [FUSE wrapper](https://github.com/dokan-dev/dokany/wiki/FUSE) that helps you to port your FUSE 
filesystems without changes
 
 ## What is Dokany
 *Dokany is a fork of Dokan 0.6.0 with bug fixes, clean change history and updated to build with latest tools.*
 
 Because the original Dokan Legacy (< 0.6.0) project is no longer maintained.
 
 Since version 0.8.0, dokany break dokan API compatibility.
-See [Choose a version](https://github.com/dokan-dev/dokany/wiki/Installation#choose-a-version) for more informations.
+See [Choose a version](https://github.com/dokan-dev/dokany/wiki/Installation#choose-a-version) for more information.
 
 **Signed x86 / x64 drivers** are provided with each release.
 
@@ -77,7 +77,7 @@ mode) which will subsequently forward the requests to the Dokan file system
 driver (dokan1.sys). By using functions provided by the Dokan user-mode
 library (dokan1.dll), file system applications are able to register callback functions to the file system driver. The file system 
 driver
-will invoke these callback routines in order to respond to the requests it received. The results of the callback routines 
will be sent back to the user program.
