
    Dokan.NET Binding

 Copyright(c) Hiroki Asakawa http://dokan-dev.net


What is Dokan.NET Binding
=========================

By using Dokan library, you can create your own file systems very easily
without writing device driver. Dokan.NET Binding is a library that allows
you to make a file system on .NET environment.


Licensing
=========

Dokan.NET Binding is distributed under a version of the "MIT License",
which is a BSD-like license. See the 'license.txt' file for details.


Environment
===========

.NET Framework 2.0 and Dokan library


How to write a file system
==========================

To make a file system, an application needs to implement DokanOperations
interface. Once implemented, you can invoke DokanNet.DokanMain function
to mount a drive. The function blocks until the file system is unmounted.
Semantics and parameters are just like Dokan library. Details are described
at 'readme.txt' file in Dokan library. See sample codes under 'sample'
directory. Administrator privileges are required to run file system
applications.


Unmounting
==========

Just run the bellow command or your file system application call DokanNet.Unmount
to unmount a drive.

   > dokanctl.exe /u DriveLetter

