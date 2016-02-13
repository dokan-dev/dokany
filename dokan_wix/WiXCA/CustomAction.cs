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

        /// <summary>
        /// Delete bundle items that depend on the current product
        /// </summary>
        [CustomAction]
        public static ActionResult DeleteBundleItems(Session session)
        {
            session.Log("DeleteBundleItems initiated");
            string myProductCode = session.CustomActionData["MYPRODUCTCODE"];
            session.Log("myProductCode=" + myProductCode);
            string myProviderKey = session.CustomActionData["MYPROVIDERKEY"];
            session.Log("myProviderKey=" + myProviderKey);

            mySession = session;

            if (!CheckAdminRights())
            {
                return ActionResult.NotExecuted;
            }

            try
            {
                string providerDependencyKey = @"SOFTWARE\Classes\Installer\Dependencies\" + myProviderKey;
                if (RegkeyExists("HKLM", providerDependencyKey))
                {
                    if (DeleteRegkeyCheck("HKLM", providerDependencyKey))
                    {
                        session.Log("Provider dependency Regkey deleted");
                    }
                }
                else
                {
                    session.Log("Provider dependencies Regkey does not exist: " + providerDependencyKey);
                }

                string productDependenciesKey = @"SOFTWARE\Classes\Installer\Dependencies\" + myProductCode + @"\Dependents";
                if (RegkeyExists("HKLM", productDependenciesKey))
                {
                    ArrayList myBundles = FindSubkeys("HKLM", productDependenciesKey);
                    session.Log("Found dependent bundles: " + myBundles.Count);
                    if (myBundles.Count > 0)
                    {
                        foreach (string myBundle in myBundles)
                        {
                            session.Log("Removing items for bundle: " + myBundle);
                            RemoveBAEntries(myProductCode, myBundle);
                        }
                    }
                    string productDependencyKey = @"SOFTWARE\Classes\Installer\Dependencies\" + myProductCode;
                    if (DeleteRegkeyCheck("HKLM", productDependencyKey))
                    {
                        session.Log("Product dependency Regkey deleted");
                    }
                }
                else
                {
                    session.Log("Product dependencies Regkey does not exist: " + productDependenciesKey);
                }
            }
            catch (Exception e)
            {
                session.Log(e.ToString());
            }
            session.Log("DeleteBundleItems completed");
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

        /// <summary>
        /// Add a regkey and optional regvalue
        /// </summary>
        /// <returns><code>true</code> if an uninstall entry was found</returns>
        public static bool AddRegkeyEntry(string regbase, string subkeyname, string valuename, string valuecontent)
        {
            bool retval = false;
            RegistryKey subKey = null;
            RegistryKey baseKey = null;

            baseKey = GetBaseKey(regbase);

            try
            {
                mySession.Log("AddRegkeyEntry creates Regkey: " + regbase + @"\" + subkeyname);
                subKey = baseKey.CreateSubKey(subkeyname, RegistryKeyPermissionCheck.ReadWriteSubTree);

                if (subKey != null)
                {
                    if (!String.IsNullOrEmpty(valuecontent))
                    {
                        mySession.Log("AddRegkeyEntry creates value: " + valuename + " content: " + valuecontent);
                        subKey.SetValue(valuename, valuecontent);
                    }
                    subKey.Close();
                    retval = true;
                }
                baseKey.Close();
            }
            catch (Exception ex)
            {
                mySession.Log("Exception raised in AddRegkeyEntry while attempting to add Regkey: " + regbase + @"\" + subkeyname);
                mySession.Log("Exception message: " + ex.Message);
            }
            return retval;
        }

        /// <summary>
        /// Add a regkey and optional regvalue
        /// </summary>
        /// <returns><code>true</code> if an uninstall entry was found</returns>
        public static bool AddRegkeyEntry(string regbase, string subkeyname, string valuename, int valuecontent)
        {
            bool retval = false;
            RegistryKey subKey = null;
            RegistryKey baseKey = null;

            baseKey = GetBaseKey(regbase);

            try
            {
                mySession.Log("AddRegkeyEntry creates Regkey: " + regbase + @"\" + subkeyname);
                subKey = baseKey.CreateSubKey(subkeyname, RegistryKeyPermissionCheck.ReadWriteSubTree);

                if (subKey != null)
                {
                    mySession.Log("AddRegkeyEntry creates value: " + valuename + " content: " + valuecontent.ToString());
                    subKey.SetValue(valuename, valuecontent, RegistryValueKind.DWord);
                    subKey.Close();
                    retval = true;
                }
                baseKey.Close();
            }
            catch (Exception ex)
            {
                mySession.Log("Exception raised in AddRegkeyEntry while attempting to add Regkey: " + regbase + @"\" + subkeyname);
                mySession.Log("Exception message: " + ex.Message);
            }
            return retval;
        }

        /// <summary>
        /// Return valuecontent for valuename if it exists
        /// </summary>
        /// <returns><code>string</code> valuecontent if value exists</returns>
        public static string GetRegvalueContent(string regbase, string subkeyname, string valuename)
        {
            string valuecontent = String.Empty;
            string retval = String.Empty;
            RegistryKey subKey = null;
            RegistryKey baseKey = null;

            baseKey = GetBaseKey(regbase);

            try
            {
                subKey = baseKey.OpenSubKey(subkeyname, RegistryKeyPermissionCheck.ReadSubTree, RegistryRights.ReadKey);

                if (subKey != null)
                {
                    if (!String.IsNullOrEmpty(valuename))
                    {
                        string myregvaluecontent = String.Empty;
                        if (subKey.GetValue(valuename) != null)
                        {
                            myregvaluecontent = subKey.GetValue(valuename, String.Empty).ToString();
                        }

                        if (!String.IsNullOrEmpty(myregvaluecontent))
                        {
                            retval = myregvaluecontent;
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                mySession.Log("Exception raised in GetRegvalueContent while attempting to read Regkey: " + regbase + @"\" + subkeyname + " value: " + valuename);
                mySession.Log("Exception message: " + ex.Message);
            }
            return retval;
        }

        public static bool DeleteRegkeyCheck(string regbase, string delsubKey)
        {
            RegistryKey baseKey = null;
            RegistryKey subKey = null;

            try
            {
                baseKey = GetBaseKey(regbase);
                if (baseKey == null)
                {
                    return false;
                }

                subKey = baseKey.OpenSubKey(delsubKey, RegistryKeyPermissionCheck.ReadWriteSubTree, RegistryRights.ReadKey | RegistryRights.Delete | RegistryRights.FullControl);
                if (subKey != null)
                {
                    //mySession.Log("Attempting to delete regkey: " + regbase + @"\" + delsubKey);
                    baseKey.DeleteSubKeyTree(delsubKey, true);
                    subKey.Close();
                    baseKey.Close();
                    mySession.Log("Deleted Regkey: " + regbase + @"\" + delsubKey);
                }
                else
                {
                    mySession.Log("Could not delete Regkey: " + regbase + @"\" + delsubKey);
                }
            }
            catch (Exception ex)
            {
                mySession.Log("Exception raised while attempting to delete Regkey: " + regbase + @"\" + delsubKey);
                mySession.Log("Exception message: " + ex.Message);
                return false;
            }
            return true;
        }

        /// <summary>
        /// Check if regkey exists
        /// </summary>
        /// <returns><code>true</code> if an uninstall entry was found</returns>
        public static bool RegkeyExists(string regbase, string subkeyname)
        {
            bool retval = false;
            RegistryKey subKey = null;
            RegistryKey baseKey = null;

            baseKey = GetBaseKey(regbase);

            try
            {
                subKey = baseKey.OpenSubKey(subkeyname, RegistryKeyPermissionCheck.ReadSubTree, RegistryRights.ReadKey);

                if (subKey != null)
                {
                    retval = true;
                }
            }
            catch (Exception ex)
            {
                mySession.Log("Exception raised in RegkeyExists while attempting to check Regkey: " + regbase + @"\" + subkeyname);
                mySession.Log("Exception message: " + ex.Message);
            }
            return retval;
        }

        /// <summary>
        /// Check if regvalue exists
        /// </summary>
        /// <returns><code>true</code> if regvalue entry exists</returns>
        public static bool RegvalueExists(string regbase, string subkeyname, string valuename, string valuecontent)
        {
            bool retval = false;
            RegistryKey subKey = null;
            RegistryKey baseKey = null;

            baseKey = GetBaseKey(regbase);

            try
            {
                subKey = baseKey.OpenSubKey(subkeyname, RegistryKeyPermissionCheck.ReadSubTree, RegistryRights.ReadKey);

                if (subKey != null)
                {
                    if (!String.IsNullOrEmpty(valuename))
                    {
                        string myregvaluecontent = String.Empty;
                        if (subKey.GetValue(valuename) != null)
                        {
                            myregvaluecontent = subKey.GetValue(valuename, String.Empty).ToString();
                        }

                        if (!String.IsNullOrEmpty(myregvaluecontent))
                        {
                            // true if any content is allowed or if it matches
                            if (String.IsNullOrEmpty(valuecontent) || myregvaluecontent.ToUpper() == valuecontent.ToUpper())
                            {
                                retval = true;
                            }
                            else
                                retval = false;
                        }
                        else
                            retval = false;
                    }
                    else retval = false;
                }
            }
            catch (Exception ex)
            {
                mySession.Log("Exception raised in RegvalueExists while attempting to check Regkey: " + regbase + @"\" + subkeyname);
                mySession.Log("Exception message: " + ex.Message);
            }
            return retval;
        }

        public static ArrayList FindSubkeys(string regbase, string subkeyname)
        {
            string[] mykeynames = { "" };
            ArrayList foundBundles = new ArrayList();

            try
            {
                subkeyname = TrailingBackslash(subkeyname);
                RegistryKey baseKey = GetBaseKey(regbase);
                if (baseKey == null)
                {
                    return null;
                }

                RegistryKey sk1 = baseKey.OpenSubKey(subkeyname, RegistryKeyPermissionCheck.ReadSubTree, RegistryRights.ReadKey);

                if (sk1 != null)
                {
                    mykeynames = sk1.GetSubKeyNames();
                    foreach (string mykeyname in mykeynames)
                    {
                        if (!String.IsNullOrEmpty(mykeyname) && IsProductCode(mykeyname))
                        {
                            foundBundles.Add(mykeyname);
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                mySession.Log("Exception raised while attempting to open Regkey: " + regbase + @"\" + subkeyname);
                mySession.Log("Exception message: " + ex.Message);
            }
            return foundBundles;
        } 

        public static RegistryKey GetBaseKey(string regbase)
        {
            RegistryKey baseKey = null;
            RegistryView regView = RegistryView.Registry32; 

            if (Environment.Is64BitOperatingSystem)
            {
                regView = RegistryView.Registry64;
            }

            if (regbase == "HKLM")
            {
                baseKey = RegistryKey.OpenBaseKey(Microsoft.Win32.RegistryHive.LocalMachine, regView);
            }
            else if (regbase == "HKCR")
            {
                baseKey = RegistryKey.OpenBaseKey(Microsoft.Win32.RegistryHive.ClassesRoot, regView);
            }
            else if (regbase == "HKCU")
            {
                baseKey = RegistryKey.OpenBaseKey(Microsoft.Win32.RegistryHive.CurrentUser, regView);
            }
            else if (regbase == "HKU")
            {
                baseKey = RegistryKey.OpenBaseKey(Microsoft.Win32.RegistryHive.Users, regView);
            }

            return baseKey;
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

        public static string GetExecutableProductInfo(string exefilepath, ref string productname)
        {
            string exeversion = String.Empty;
            productname = String.Empty;

            try
            {
                if (MyFileExists(exefilepath))
                {
                    FileVersionInfo myFileVersionInfo = FileVersionInfo.GetVersionInfo(exefilepath);
                    exeversion = myFileVersionInfo.ProductVersion;
                    productname = myFileVersionInfo.ProductName;
                }
            }
            catch (Exception ex)
            {
                mySession.Log("Exception raised in GetExecutableProductInfo while attempting to open: " + exefilepath);
                mySession.Log("Exception message: " + ex.Message);
            }
            return exeversion;
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
                System.Diagnostics.ProcessStartInfo procStartInfo = new System.Diagnostics.ProcessStartInfo(command);
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
                System.Diagnostics.Process proc = new System.Diagnostics.Process();
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

        public static void ExecuteProcessInstantly(string command, string clparameter)
        {
            if (!MyFileExists(command))
            {
                mySession.Log("ExecuteProcessInstantly command does not exist: " + command);
                return;
            }

            try
            {
                System.Diagnostics.ProcessStartInfo procStartInfo = new System.Diagnostics.ProcessStartInfo(command);
                mySession.Log("Executing: " + command + " " + clparameter);

                procStartInfo.Arguments = clparameter;
                procStartInfo.RedirectStandardOutput = false;
                procStartInfo.UseShellExecute = false;

                System.IO.DirectoryInfo myDirectory = new DirectoryInfo(command);
                procStartInfo.WorkingDirectory = myDirectory.Parent.FullName;
                //procStartInfo.CreateNoWindow = true;
                //procStartInfo.WindowStyle = ProcessWindowStyle.Hidden;
                System.Diagnostics.Process proc = new System.Diagnostics.Process();
                proc.StartInfo = procStartInfo;
                proc.Start();
            }
            catch (Exception ex)
            {
                mySession.Log("Exception in ExecuteProcessInstantly: " + command);
                mySession.Log("Exception message: " + ex.Message);
            }
        }

        public static int RemoveBAEntries(string productCode, string baUninstallCode)
        {
            mySession.Log("RemoveThisBAEntries for ProductCode: " + productCode + " BAUninstallCode: " + baUninstallCode);
            int retval = 0;

            retval += DeleteBAUninstallEntry(baUninstallCode);
            retval += DeletePackageCache(baUninstallCode);
            if (!String.IsNullOrEmpty(productCode))
            {
                retval += DeleteProductDependentsBA(productCode, baUninstallCode);
            }
            mySession.Log("Removed items: " + retval);
            return retval;
        }

        public static int DeleteBAUninstallEntry(string baUninstallCode)
        {
            int retval = 0;
            string subkeyname = String.Empty;
            mySession.Log(@"Before deleting BA uninstall entry: " + baUninstallCode);

            if (!String.IsNullOrEmpty(baUninstallCode))
            {
                subkeyname = @"SOFTWARE\Wow6432Node\Microsoft\Windows\CurrentVersion\Uninstall\" + baUninstallCode;
                mySession.Log(@"Deleting BA uninstall entry: HKLM\" + subkeyname);
                retval += DeleteRegkey("HKLM", subkeyname);
            }
            return retval;
        }

        public static int DeleteProductDependentsBA(string productcode, string baUninstallCode)
        {
            int retval = 0;
            string subkeyname = String.Empty;
            mySession.Log(@"Before deleting dependents for ProductCode: " + productcode + " with BACode: " + baUninstallCode);

            if (!String.IsNullOrEmpty(productcode) && !String.IsNullOrEmpty(baUninstallCode))
            {
                subkeyname = @"SOFTWARE\Classes\Installer\Dependencies\" + productcode + @"\Dependents\" + baUninstallCode;
                mySession.Log(@"Deleting BA dependency entry: HKLM\" + subkeyname);
                retval += DeleteRegkey("HKLM", subkeyname);

                subkeyname = @"Installer\Dependencies\" + productcode + @"\Dependents\" + baUninstallCode;
                mySession.Log(@"Deleting BA dependency entry: HKCR\" + subkeyname);
                retval += DeleteRegkey("HKCR", subkeyname);
            }
            return retval;
        }

        public static int DeleteDependencies(string productcode)
        {
            int retval = 0;
            string subkeyname = String.Empty;

            mySession.Log(@"DeleteDependencies ProductCode=" + productcode);
            if (!String.IsNullOrEmpty(productcode))
            {
                string convertcode = ConvertProductGUIDToCode(productcode);
                if (String.IsNullOrEmpty(convertcode))
                {
                    mySession.Log("DeleteDependencies convertcode could not be created from: " + productcode);
                    return 0;
                }
                string deletekey = convertcode;

                if (!String.IsNullOrEmpty(deletekey))
                {
                    subkeyname = @"Installer\Dependencies\" + deletekey;
                    retval += DeleteRegkey("HKCR", subkeyname);
                    subkeyname = @"Installer\Features\" + deletekey;
                    retval += DeleteRegkey("HKCR", subkeyname);
                    subkeyname = @"Installer\Products\" + deletekey;
                    retval += DeleteRegkey("HKCR", subkeyname);

                    subkeyname = @"SOFTWARE\Classes\Installer\Dependencies\" + deletekey;
                    retval += DeleteRegkey("HKLM", subkeyname);
                    subkeyname = @"SOFTWARE\Classes\Installer\Features\" + deletekey;
                    retval += DeleteRegkey("HKLM", subkeyname);
                    subkeyname = @"SOFTWARE\Classes\Installer\Products\" + deletekey;
                    retval += DeleteRegkey("HKLM", subkeyname);
                }

                deletekey = productcode;
                if (!String.IsNullOrEmpty(deletekey))
                {
                    subkeyname = @"Installer\Dependencies\" + deletekey;
                    retval += DeleteRegkey("HKCR", subkeyname);
                    subkeyname = @"Installer\Features\" + deletekey;
                    retval += DeleteRegkey("HKCR", subkeyname);
                    subkeyname = @"Installer\Products\" + deletekey;
                    retval += DeleteRegkey("HKCR", subkeyname);

                    subkeyname = @"SOFTWARE\Classes\Installer\Dependencies\" + deletekey;
                    retval += DeleteRegkey("HKLM", subkeyname);
                    subkeyname = @"SOFTWARE\Classes\Installer\Features\" + deletekey;
                    retval += DeleteRegkey("HKLM", subkeyname);
                    subkeyname = @"SOFTWARE\Classes\Installer\Products\" + deletekey;
                    retval += DeleteRegkey("HKLM", subkeyname);
                }
            }
            return retval;
        }

        public static int DeletePackageCache(string productcode)
        {
            if (String.IsNullOrEmpty(productcode))
            {
                return 0;
            }

            string packagecache = TrailingBackslash(Environment.GetEnvironmentVariable("ProgramData")) + @"Package Cache";

            if (!MyDirExists(packagecache))
            {
                mySession.Log(@"PackageCache folder does not exist");
                return 0;
            }

            int retval = 0;
            string[] subdirectoryEntries = Directory.GetDirectories(packagecache);
            foreach (string subdirectory in subdirectoryEntries)
            {
                if (subdirectory.ToUpper().Contains(productcode.ToUpper()))
                {
                    mySession.Log(@"DeletePackageCache deleting Package Cache folder: " + subdirectory);
                    try
                    {
                        DeleteDirectory(subdirectory, true);
                    }
                    catch (Exception ex)
                    {
                        mySession.Log("Exception raised while attempting to delete folder: " + subdirectory);
                        mySession.Log("Exception message: " + ex.Message);
                    }
                    ++retval;
                }
            }
            return retval;
        }

        public static string ConvertProductGUIDToCode(string inguid)
        {
            // product GUID: {42BCB204-AF59-4021-AB10-FB95D8514D00}
            // product code: 402BCB2495FA1204BA01BF598D15D400 

            string outguid = String.Empty;

            if (!IsProductCode(inguid))
                return outguid;

            outguid += inguid[8];
            outguid += inguid[7];
            outguid += inguid[6];
            outguid += inguid[5];
            outguid += inguid[4];
            outguid += inguid[3];
            outguid += inguid[2];
            outguid += inguid[1];

            outguid += inguid[13];
            outguid += inguid[12];
            outguid += inguid[11];
            outguid += inguid[10];

            outguid += inguid[18];
            outguid += inguid[17];
            outguid += inguid[16];
            outguid += inguid[15];

            outguid += inguid[21];
            outguid += inguid[20];
            outguid += inguid[23];
            outguid += inguid[22];

            outguid += inguid[26];
            outguid += inguid[25];
            outguid += inguid[28];
            outguid += inguid[27];
            outguid += inguid[30];
            outguid += inguid[29];
            outguid += inguid[32];
            outguid += inguid[31];
            outguid += inguid[34];
            outguid += inguid[33];
            outguid += inguid[36];
            outguid += inguid[35];

            return outguid;
        }

        public static string ConvertProductCodeToGUID(string inguid)
        {
            // product code: 946F9E309CBF07A41A8176D31119D59D 
            // product GUID: {03E9F649-FBC9-4A70-A118-673D11915DD9}

            string outguid = String.Empty;

            if (inguid.Length != 32)
                return outguid;

            outguid += "{";

            outguid += inguid[7];
            outguid += inguid[6];
            outguid += inguid[5];
            outguid += inguid[4];
            outguid += inguid[3];
            outguid += inguid[2];
            outguid += inguid[1];
            outguid += inguid[0];

            outguid += "-";

            outguid += inguid[11];
            outguid += inguid[10];
            outguid += inguid[9];
            outguid += inguid[8];

            outguid += "-";

            outguid += inguid[15];
            outguid += inguid[14];
            outguid += inguid[13];
            outguid += inguid[12];

            outguid += "-";

            outguid += inguid[17];
            outguid += inguid[16];
            outguid += inguid[19];
            outguid += inguid[18];

            outguid += "-";

            outguid += inguid[21];
            outguid += inguid[20];
            outguid += inguid[23];
            outguid += inguid[22];
            outguid += inguid[25];
            outguid += inguid[24];
            outguid += inguid[27];
            outguid += inguid[26];
            outguid += inguid[29];
            outguid += inguid[28];
            outguid += inguid[31];
            outguid += inguid[30];

            outguid += "}";

            return outguid;
        }

        public static bool IsProductCode(string productcode)
        {
            bool retval = false;
            if (!String.IsNullOrEmpty(productcode) && productcode.Length == 38)
            {
                retval = productcode[0] == '{' && productcode[37] == '}';
            }

            return retval;
        }

        public static int DeleteRegkey(string regbase, string delsubKey)
        {
            int retval = 0;
            RegistryKey baseKey = null;
            RegistryKey subKey = null;

            try
            {
                baseKey = GetBaseKey(regbase);
                if (baseKey == null)
                {
                    return retval;
                }

                subKey = baseKey.OpenSubKey(delsubKey, RegistryKeyPermissionCheck.ReadWriteSubTree, RegistryRights.ReadKey | RegistryRights.Delete | RegistryRights.FullControl);
                if (subKey != null)
                {
                    baseKey.DeleteSubKeyTree(delsubKey, true);
                    subKey.Close();
                    baseKey.Close();
                    retval++;
                    mySession.Log("Deleted Regkey: " + regbase + @"\" + delsubKey);
                }
                else
                {
                    //mySession.Log("Could not delete regkey: " + regbase + @"\" + delsubKey);
                }
            }
            catch (Exception ex)
            {
                mySession.Log("Exception raised while attempting to delete Regkey: " + regbase + @"\" + delsubKey);
                mySession.Log("Exception message: " + ex.Message);
            }
            return retval;
        }
    }
}

