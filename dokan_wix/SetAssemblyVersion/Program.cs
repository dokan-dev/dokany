using System;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace SetAssemblyVersion
{
    internal class Program
    {
        private const string ProductVersionRegex =
            @"\b[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,5}\.[0-9]{1,5}\b";

        private static int Main(string[] args)
        {
            if (args.Length < 3)
            {
                ShowHelp();
                return (int) EReturnCode.MissingParametersHelpShown;
            }

            // Expected content of changelog.md file:

            //--------------------------------
            // ## [Unreleased] - 1.0.0.4000
            // ## [1.0.0.5000] - 2015-12-10
            //--------------------------------

            var productVersion = ReadVersion(args[0]);
            if (productVersion == new Version())
                return (int) EReturnCode.VersionMissingOrInvalid;

            Console.WriteLine("");

            var version =
                $"{productVersion.Major}.{productVersion.Minor}.{productVersion.Build}.{productVersion.Revision}";
            var versionComma =
                $"{productVersion.Major},{productVersion.Minor},{productVersion.Build},{productVersion.Revision}";

                var xmlFile = args[1].Replace("\"", "").Trim();

                var result = ModifyProductParametersXml(xmlFile, productVersion);
                if ((EReturnCode) result == EReturnCode.None)
                {
                    var files = Directory.GetFiles(args[2], "*.rc", SearchOption.AllDirectories);
                    Console.WriteLine("Update version in RC Files");

                    foreach (var file in files)
                    {
                        Console.WriteLine("RC File {0} version updated.", file);
                        var rcfile = File.ReadAllText(file);
                        rcfile = Regex.Replace(rcfile, @"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+", version);
                        rcfile = Regex.Replace(rcfile, @"[0-9]+,[0-9]+,[0-9]+,[0-9]+", versionComma);

                        File.WriteAllText(file, rcfile);
                    }

                    Console.WriteLine("Update build VS define versions");
                    var props = File.ReadAllText(args[2] + @"\Dokan.props");
                    var majorApiDefineVersionString = $@"<DOKANAPIVersion>{productVersion.Major}</DOKANAPIVersion>";
                    props = Regex.Replace(props, @"<DOKANAPIVersion>[0-9]+<\/DOKANAPIVersion>",
                        majorApiDefineVersionString);
                    var defineVersionString =
                        $@"<DOKANVersion>{productVersion.Major}.{productVersion.Minor}.{productVersion.Build}</DOKANVersion>";
                    props = Regex.Replace(props, @"<DOKANVersion>[0-9]+.[0-9]+.[0-9]+<\/DOKANVersion>",
                        defineVersionString);
                    File.WriteAllText(args[2] + @"\Dokan.props", props);
                }

                return result;
        }

        /// <summary>
        ///     Find version information in a "version.txt"
        /// </summary>
        /// <param name="file"></param>
        /// <returns></returns>
        private static Version ReadVersion(string file)
        {
            var fi = new FileInfo(file);
            if (!fi.Exists)
                return new Version();

            using (var reader = new StreamReader(file))
            {
                var regExVersion = new Regex(ProductVersionRegex, RegexOptions.IgnoreCase);

                var line = 0;
                while (reader.Peek() != 0 && line < 10) // find version in first 5 lines 
                {
                    line++;
                    var data = reader.ReadLine();
                    if (string.IsNullOrEmpty(data))
                        continue;

                    var m = regExVersion.Match(data);
                    if (!string.IsNullOrEmpty(m.Value))
                        return Version.Parse(m.Value);
                }
            }

            return new Version();
        }

        private static void ShowHelp()
        {
            Console.WriteLine("Usage: SetAssemblyVersion.exe <changelogfile> <xmlfile> <rootfolder>");
        }

        /// <summary>
        ///     Write version info into version.xml
        /// </summary>
        /// <param name="xmlFile">Xml definition file for WiX</param>
        /// <param name="productVer">product Version</param>
        /// <param name="buildDate">product Release/Build Date</param>
        /// <returns></returns>
        private static int ModifyProductParametersXml(string xmlFile, Version productVer)
        {
            Console.WriteLine("Modifying Version xml file: " + xmlFile);

            if (!File.Exists(xmlFile))
                return (int) EReturnCode.FileIsMissing;

            var lines = File.ReadAllLines(xmlFile);

            var todayDate = DateTime.Now;

            var dateString = todayDate.ToString("yyyyMMddHHmmss").Substring(2);

            for (var i = 0; i < lines.Length; i++)
            {
                string value;
                var line = lines[i];

                if (!line.Contains("=\""))
                    continue;

                var parts = line.Split('=');
                if (parts.Length < 1)
                    continue;

                const string cLineFormat = "{0}=\"{1}\" ?>";

                if (line.ToLower().Contains("productcodex86"))
                {
                    var myVersion = productVer.Major.ToString("X2") + productVer.Minor.ToString("X2") + "-" +
                                    productVer.Build.ToString("X4") + "-" + dateString;
                    value = parts[1];

                    // Format ProductCode="{65A3A986-3DC3-mjmi-buld-yyMMddHHmmss}" ?>

                    value = value.Substring(0, 16) + myVersion;
                    line = $"{parts[0]}={value}" + "}" + "\"" + " ?>";
                }
                else if (line.ToLower().Contains("productcodex64"))
                {
                    var myVersion = productVer.Major.ToString("X2") + productVer.Minor.ToString("X2") + "-" +
                                    productVer.Build.ToString("X4") + "-" + dateString;
                    value = parts[1];

                    // Format ProductCode="{65A3A964-3DC3-mjmi-buld-yyMMddHHmmss}" ?>

                    value = value.Substring(0, 16) + myVersion;
                    line = $"{parts[0]}={value}" + "}" + "\"" + " ?>";
                }
                else if (line.ToLower().Contains("majorversion"))
                {
                    value = $"{productVer.Major}";
                    line = string.Format(cLineFormat, parts[0], value);
                }
                else if (line.ToLower().Contains("baseversion"))
                {
                    value = $"{productVer.Major}.{productVer.Minor}.{productVer.Build}";
                    line = string.Format(cLineFormat, parts[0], value);
                }
                else if (line.ToLower().Contains("buildversion"))
                {
                    value = productVer.Revision.ToString();
                    // value = yearOnly;
                    line = string.Format(cLineFormat, parts[0], value);
                }

                lines[i] = line;
            }

            File.WriteAllLines(xmlFile, lines);

            return (int) EReturnCode.None;
        }

        /// <summary>
        ///     Return values
        /// </summary>
        private enum EReturnCode
        {
            None = 0,
            MissingParametersHelpShown = 1,
            VersionMissingOrInvalid = -1,
            FileTypePassedUnknown = -2,
            FileIsMissing = -3,
            NoValidDateTimeForSetup = -4,
            UnknownError = -5,
            DestinationPathNotExisting = -6,
            SourcePathNotExisting = -7,
            BuildPublishingPathNotExisting = -8,
            SetupNotCopiedToBuildPath = -9,
            SetupInBuildPathAlreadyExisting = -10,
            SetupInSourcePathMissing = -11,
            MissingProcessingInstruction = -12,
            RootFolderMissing = -13
        }
    } //class
} //namespace