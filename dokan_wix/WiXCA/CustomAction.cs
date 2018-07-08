using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using Microsoft.Deployment.WindowsInstaller;
using Microsoft.Win32;

namespace WiXCA
{
    public class CustomActions
    {
        private static Session mySession;

        /// <summary>
        /// Execute dokanctl /i a and dokanctl /i n
        /// </summary>
        [CustomAction]
        public static ActionResult ExecuteInstall(Session session)
        {
            session.Log("ExecuteInstall initiated");
            string installFolder = String.Empty;
            try
            {
                installFolder = session.CustomActionData["INSTALLFOLDER"];
            }
            catch (Exception e)
            {
                session.Log("ExecuteInstall Exception trying to read CustomActionData: " + e.ToString());
                return ActionResult.NotExecuted;
            }
            session.Log("InstallPath=" + installFolder);

            mySession = session;

            if (!CheckAdminRights())
            {
                session.Log("No admin privileges, terminating CustomAction now");
                return ActionResult.NotExecuted;
            }

            try
            {
                if (!MyDirExists(installFolder))
                {
                    session.Log("ExecuteInstall installdir not found: " + installFolder);
                    return ActionResult.NotExecuted;
                }
                string dokanctlPath = Path.Combine(installFolder, "dokanctl.exe");
                if (!MyFileExists(dokanctlPath))
                {
                    session.Log("ExecuteInstall file not found: " + dokanctlPath);
                    return ActionResult.NotExecuted;
                }
                
                // string executeResult = ExecuteProcessAndWaitForResult(dokanctlPath, @"/i a");
                // session.Log("ExecuteInstall '" + dokanctlPath + "' result:\n" + executeResult);

                string executeResult2 = ExecuteProcessAndWaitForResult(dokanctlPath, @"/i n");
                session.Log("ExecuteInstall '" + dokanctlPath + "' result:\n " + executeResult2);

            }
            catch (Exception e)
            {
                session.Log("ExecuteInstall Exception: " + e.ToString());
                return ActionResult.NotExecuted;
            }
            session.Log("ExecuteInstall completed");
            return ActionResult.Success;
        }

        /// <summary>
        /// Execute dokanctl /r n and dokanctl /r a
        /// </summary>
        [CustomAction]
        public static ActionResult ExecuteUninstall(Session session)
        {
            session.Log("ExecuteUninstall initiated");
            string installFolder = String.Empty;
            try
            {
                installFolder = session.CustomActionData["INSTALLFOLDER"];
            }
            catch (Exception e)
            {
                session.Log("ExecuteUninstall Exception trying to read CustomActionData: " + e.ToString());
                return ActionResult.NotExecuted;
            }
            session.Log("InstallPath=" + installFolder);

            mySession = session;

            if (!CheckAdminRights())
            {
                session.Log("No admin privileges, terminating CustomAction now");
                return ActionResult.NotExecuted;
            }

            try
            {
                if (!MyDirExists(installFolder))
                {
                    session.Log("ExecuteUninstall installdir not found: " + installFolder);
                    return ActionResult.NotExecuted;
                }
                string dokanctlPath = Path.Combine(installFolder, "dokanctl.exe");
                if (!MyFileExists(dokanctlPath))
                {
                    session.Log("ExecuteUninstall file not found: " + dokanctlPath);
                    return ActionResult.NotExecuted;
                }

                string executeResult = ExecuteProcessAndWaitForResult(dokanctlPath, @"/r n");
                session.Log("ExecuteUninstall '" + dokanctlPath + "' result:\n" + executeResult);

                // string executeResult2 = ExecuteProcessAndWaitForResult(dokanctlPath, @"/r a");
                // session.Log("ExecuteUninstall '" + dokanctlPath + "' result:\n" + executeResult2);

            }
            catch (Exception e)
            {
                session.Log("ExecuteUninstall Exception: " + e.ToString());
                return ActionResult.NotExecuted;
            }
            session.Log("ExecuteUninstall completed");
            return ActionResult.Success;
        }

        public static bool CheckAdminRights()
        {
            bool isAdmin = new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator) ? true : false;
            if (!isAdmin)
            {
                mySession.Log("Process is executing without admin privileges");
            }
            else
            {
                mySession.Log("Process is running with admin privileges");
            }
            return isAdmin;
        } 

        public static string TrailingBackslash(string inputstring)
        {
            //inputstring = inputstring.Replace(@"\\", @"\");
            if (String.IsNullOrEmpty(inputstring))
            {
                return inputstring;
            }

            if (inputstring[inputstring.Length - 1] != '\\')
            {
                return inputstring + @"\";
            }
            else
            {
                return inputstring;
            }
        }

        public static void DeleteDirectory(string path, bool recursive)
        {
            if (String.IsNullOrEmpty(path) || path.Length < 5)
            {
                mySession.Log("DeleteDirectory error, path is invalid: " + path);
                return;
            }
            else
            {
                path = TrailingBackslash(path);
            }

            string windowspath = TrailingBackslash(Environment.GetEnvironmentVariable("windir"));
            string programfiles = TrailingBackslash(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles).ToUpper());
            string programfilesX86 = TrailingBackslash(Environment.GetEnvironmentVariable("ProgramFiles(x86)").ToUpper());
            string packagecache = TrailingBackslash(Environment.GetEnvironmentVariable("ProgramData").ToUpper()) + @"PACKAGE CACHE";
            string systemtempdir = TrailingBackslash(System.IO.Path.GetTempPath());

            if ((path.ToUpper().Contains(@":\") && path.Length < 4)
                || path.ToUpper() == windowspath.ToUpper()
                || path.ToUpper() == programfiles.ToUpper()
                || path.ToUpper() == programfilesX86.ToUpper()
                || path.ToUpper() == packagecache.ToUpper()
                || path.ToUpper() == systemtempdir.ToUpper()
                )
            {
                mySession.Log("Fatal Error, was attempting to delete: '" + path + "'"); 
                return;
            }

            if (!MyDirExists(path))
            {
                return;
            }

            System.IO.FileAttributes attr;
            try
            {
                attr = File.GetAttributes(path);
                if ((attr & System.IO.FileAttributes.ReadOnly) == System.IO.FileAttributes.ReadOnly)
                {
                    File.SetAttributes(path, attr ^ System.IO.FileAttributes.ReadOnly);
                }
                if ((attr & System.IO.FileAttributes.Hidden) == System.IO.FileAttributes.Hidden)
                {
                    File.SetAttributes(path, attr ^ System.IO.FileAttributes.Hidden);
                }
                if ((attr & System.IO.FileAttributes.System) == System.IO.FileAttributes.System)
                {
                    File.SetAttributes(path, attr ^ System.IO.FileAttributes.System);
                }

                if (recursive)
                {
                    var subfolders = Directory.GetDirectories(path);
                    foreach (var s in subfolders)
                    {
                        mySession.Log("DeleteDirectory recurse into: '" + s + "'");
                        DeleteDirectory(s, recursive);
                    }
                }
            }
            catch (Exception ex)
            {
                mySession.Log("Exception while trying to DeleteDirectory SetAttributes in: '" + path + "'");
                mySession.Log("Exception message: " + ex.Message);
            }

            try
            {
                // Get all files of the folder
                var files = Directory.GetFiles(path);
                foreach (var f in files)
                {
                    File.Delete(f);
                }

                mySession.Log("DeleteDirectory: '" + path + "'");
                Directory.Delete(path);
            }
            catch (Exception ex)
            {
                mySession.Log("Exception while trying to DeleteDirectory deleting files in: '" + path + "'");
                mySession.Log("Exception message: " + ex.Message);
            }
        }

        public static bool MyDirExists(string foldernname)
        {
            if (!String.IsNullOrEmpty(foldernname))
            {
                DirectoryInfo fi = new DirectoryInfo(foldernname);
                return fi.Exists;
            }
            return false;
        }

        public static bool MyFileExists(string filename)
        {
            if (!String.IsNullOrEmpty(filename))
            {
                FileInfo fi = new FileInfo(filename);
                return fi.Exists;
            }
            return false;
        }

        public static string ExecuteProcessAndWaitForResult(string command, string clparameter)
        {
            mySession.Log("ExecuteProcessAndWaitForResult: " + command + " " + clparameter);
            string result = String.Empty;

            if (String.IsNullOrEmpty(command))
            {
                return result;
            }

            try
            {
                ProcessStartInfo procStartInfo = new ProcessStartInfo(command);
                mySession.Log("Executing: " + command + " " + clparameter);

                procStartInfo.Arguments = clparameter;
                procStartInfo.RedirectStandardOutput = true;
                procStartInfo.UseShellExecute = false;

                if (command.Contains(@":\"))
                {
                    System.IO.DirectoryInfo myDirectory = new DirectoryInfo(command);
                    procStartInfo.WorkingDirectory = myDirectory.Parent.FullName;
                }
                else
                {
                    string windowspath = Environment.GetEnvironmentVariable("windir");
                    procStartInfo.WorkingDirectory = windowspath;
                }

                procStartInfo.CreateNoWindow = true;
                procStartInfo.WindowStyle = ProcessWindowStyle.Hidden;
                Process proc = new Process();
                proc.StartInfo = procStartInfo;
                proc.Start();
                result = proc.StandardOutput.ReadToEnd();
            }
            catch (Exception ex)
            {
                mySession.Log("Exception in ExecuteProcessAndWaitForResult: " + command);
                mySession.Log("Exception message: " + ex.Message);
            }
            return result;
        }
    }
}

