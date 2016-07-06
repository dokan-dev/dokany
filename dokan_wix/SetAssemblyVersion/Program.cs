using System;
using System.Globalization;
using System.Text;
using System.IO;
using System.Text.RegularExpressions;

namespace SetAssemblyVersion
{
	class Program
	{
		const string cStartAttribute = "[assembly: ";
		const string cSearch1 = "AssemblyVersion(\"";
		const string cSearch2 = "AssemblyFileVersion(\"";
		const string cSearch3 = "AssemblyBuildDateAttribute(\"";
		const string cSearch4 = "AssemblyBuildTagAttribute(\"";
		const string cEnd = "\")]";

        const string productVersionRegex = @"\bv[0-9]{1,2}.[0-9]{1,2}.[0-9]{1,3}.[0-9]+\b";

        /// <summary>
        /// Actions
        /// </summary>
        enum EAction
		{
			Unknown,
			Assembly,
			SetupProject,
			AppConfig,
			Resource
		};

		/// <summary>
		/// Return values
		/// </summary>
		enum EReturnCode
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
            RootFolderMissing = -13,
        }

		static int Main(string[] args)
		{
			if (args.Length < 2)
			{
				ShowHelp();
				return (int)EReturnCode.MissingParametersHelpShown;
			}

            // Expected content of version.txt file:

            //--------------------------------
            // v1.0.0.12345 (RC1)
            // Build date: 20160303
            //--------------------------------

            Version productVersion = ReadVersion(args[0]);
			if (productVersion == new Version())
				return (int)EReturnCode.VersionMissingOrInvalid;

			EAction actionToDo = EAction.Unknown;

			if (args[1].Trim().ToLower() == "/setversion")
				actionToDo = EAction.SetupProject;

			else if (args[1].ToLower().EndsWith("app.config"))
				actionToDo = EAction.AppConfig;

			else if (args[1].ToLower().EndsWith("assemblyinfo.cs"))
				actionToDo = EAction.Assembly;

			else if (args[1].ToLower().EndsWith(".rc2"))
				actionToDo = EAction.Resource;

			Console.WriteLine("");

			if (actionToDo == EAction.Unknown)
				return (int)EReturnCode.FileTypePassedUnknown;

			if (actionToDo != EAction.SetupProject)
			{
				if (!File.Exists(args[1]))
					return (int)EReturnCode.FileIsMissing;

                if (args.Length < 3
                    || (args.Length > 2 && !Directory.Exists(args[3])))
                    return (int)EReturnCode.RootFolderMissing;
            }

            string version = string.Format("{0}.{1}.{2}.{3}", productVersion.Major, productVersion.Minor, productVersion.Build, productVersion.Revision);
            string versionComma = string.Format("{0},{1},{2},{3}", productVersion.Major, productVersion.Minor, productVersion.Build, productVersion.Revision);

            if (actionToDo == EAction.SetupProject)
			{
				string xmlFile = args[2].Replace("\"", "").Trim();
				DateTime buildDate = ReadBuildDate(args[0]);

				int result = ModifyProductParametersXml(xmlFile, productVersion, buildDate);
                if ((EReturnCode)result == EReturnCode.None)
                {
                    var files = Directory.GetFiles(args[3], "*.rc", SearchOption.AllDirectories);
                    Console.WriteLine("Update version in RC Files");

                    foreach (var file in files)
                    {
                        Console.WriteLine(string.Format("RC File {0} version updated.", file));
                        string rcfile = File.ReadAllText(file);
                        rcfile = Regex.Replace(rcfile, @"[0-9]{1,2}\.[0-9]{1,2}\.[0-9]{1,2}\.[0-9]{1,2}", version);
                        rcfile = Regex.Replace(rcfile, @"[0-9]{1,2},[0-9]{1,2},[0-9]{1,2},[0-9]{1,2}", versionComma);

                        File.WriteAllText(file, rcfile);
                    }

                    Console.WriteLine("Update build VS define versions");
                    string props = File.ReadAllText(args[3] + @"\Dokan.props");
                    string majorAPIDefineVersionString = string.Format(@"<DOKANAPIVersion>{0}</DOKANAPIVersion>", productVersion.Major, productVersion.Minor, productVersion.Build);
                    props = Regex.Replace(props, @"<DOKANAPIVersion>[0-9]{1,2}<\/DOKANAPIVersion>", majorAPIDefineVersionString);
                    string defineVersionString = string.Format(@"<DOKANVersion>{0}.{1}.{2}</DOKANVersion>", productVersion.Major, productVersion.Minor, productVersion.Build);
                    props = Regex.Replace(props, @"<DOKANVersion>[0-9]{1,2}.[0-9]{1,2}.[0-9]{1,3}<\/DOKANVersion>", defineVersionString);
                    File.WriteAllText(args[3] + @"\Dokan.props", props);
                }

                return result;
			}

			try
			{
				string data = string.Empty;
				string dataOrg = string.Empty;

				int bReturnVal = 0;
				int result = 0;

				using (StreamReader reader = new StreamReader(args[1], Encoding.UTF8))
				{
					data = reader.ReadToEnd();
					dataOrg = data;
				}

				if (actionToDo == EAction.Resource)
				{
					Console.WriteLine("### Set Version " + version + " of " + args[1]);
					result = ReplaceString(ref data, version.Replace('.', ','), "FILEVERSION ", "\r\n", true);
					result += ReplaceString(ref data, version.Replace('.', ','), "PRODUCTVERSION ", "\r\n", true);
					result += ReplaceString(ref data, version, "\"FileVersion\", \"", "\"", true);
					result += ReplaceString(ref data, version, "\"ProductVersion\", \"", "\"", true);

					string dllBits = "32";
					if (args.Length > 2 && (args[2].ToLower().StartsWith("x64")))
						dllBits = "64";

					result += ReplaceString(ref data, dllBits, "\"product", ".dll\"", true);
					bReturnVal = result;
				}
				else if (actionToDo == EAction.Assembly)
				{
					Console.WriteLine("### Set Version " + version + " of " + args[1]);
					result = ReplaceString(ref data, version, cSearch1, cEnd, true);
					int result2 = ReplaceString(ref data, version, cSearch2, cEnd, true);
					if (args.Length == 3 && args[2].ToLower() == "set_buildDate")
					{
						DateTime buildDate = ReadBuildDate(args[0]);
						Console.WriteLine("### Set buildDate Attribute " + buildDate + " of " + args[1]);
						ReplaceString(ref data, buildDate.ToShortDateString(), cSearch3, cEnd, true, cStartAttribute);

						string updateTag = ReadBuildTag(args[0]);
						Console.WriteLine("### Set updateTag Attribute " + updateTag + " of " + args[1]);
						ReplaceString(ref data, updateTag, cSearch4, cEnd, true, cStartAttribute);
					}

					bReturnVal = result + result2;

				}
				else if (actionToDo == EAction.AppConfig)
				{
					string updateTag = ReadBuildTag(args[0]);
					int pos = FindStringPos(data, "\"CurrentUpdateTag\"");
					if (pos > 0)
					{
						{
							Console.WriteLine("### Set CurrentUpdateTag " + updateTag + " of " + args[1]);
							result = ReplaceString(ref data, updateTag, "<value>", "</value>", pos);
						}
					}
				}

				if (actionToDo != EAction.Unknown && bReturnVal == 0 && (data != dataOrg))
				{
					using (StreamWriter writer = new StreamWriter(args[1], false, actionToDo != EAction.Resource ? System.Text.Encoding.UTF8 : System.Text.Encoding.Unicode))
					{
						writer.Write(data);
					}
				}
				return bReturnVal;
			}
			catch (Exception ex)
            {
                Console.WriteLine("Exception occurred, message: " + ex.Message);
                Console.WriteLine("Exception info: " + ex.ToString());
            }

			return (int)EReturnCode.UnknownError;
		}

		static private int FindStringPos(string data, string searchStr, int startSearch = 0)
		{
			return data.IndexOf(searchStr, startSearch, StringComparison.CurrentCulture);
		}

		static private string GetString(string data, string startsearch, string endsearch, int searchpos)
		{
			int posToSearch = searchpos;

			int pos = data.IndexOf(startsearch, posToSearch, StringComparison.CurrentCultureIgnoreCase);
			if (pos == -1)
				return string.Empty;

			posToSearch = pos + startsearch.Length;

			int pos2 = data.IndexOf(endsearch, posToSearch, StringComparison.CurrentCultureIgnoreCase);
			if (pos2 == -1)
				return string.Empty;

			return data.Substring(posToSearch, pos2 - posToSearch);
		}

		static private int ReplaceString(ref string data, string replace, string startsearch, string endsearch, int searchpos, bool bAll = false, string startAttribute = "")
		{
			bool bFound = false;
			int posToSearch = searchpos;
			while (true)
			{
				int pos = data.IndexOf(startsearch, posToSearch, StringComparison.CurrentCultureIgnoreCase);
				if (pos == -1)
					break;
				posToSearch = pos + startsearch.Length;

				int pos2 = data.IndexOf(endsearch, posToSearch, StringComparison.CurrentCultureIgnoreCase);
				if (pos2 == -1)
					break;

				data = data.Substring(0, posToSearch) + replace + data.Substring(pos2);
				bFound = true;
				if (!bAll)
					break;
			}

			if (!bFound && !string.IsNullOrEmpty(startAttribute))
			{
				data += "\r\n\r\n// SetAssemblyVersion insert following Attribute\r\n";
				data += startAttribute + startsearch + replace + endsearch;
				bFound = true;
			}

			return bFound ? 0 : -10;
		}

		static private int ReplaceString(ref string data, string replace, string startsearch, string endsearch, bool bAll = false, string startAttribute = "")
		{
			return ReplaceString(ref data, replace, startsearch, endsearch, 0, bAll, startAttribute);
		}

		/// <summary>
		/// Find version information in a "version.txt"
		/// </summary>
		/// <param name="file"></param>
		/// <returns></returns>
		static private Version ReadVersion(string file)
		{
			FileInfo fi = new FileInfo(file);
			if (!fi.Exists)
				return new Version();

			using (StreamReader reader = new StreamReader(file))
			{
				Regex regExVersion = new Regex(productVersionRegex, RegexOptions.IgnoreCase);

				int line = 0;
				while (reader.Peek() != 0 && line < 5) // find version in first 5 lines 
				{
					line++;
					string data = reader.ReadLine();
					if (string.IsNullOrEmpty(data))
						continue;

                    Match m = regExVersion.Match(data);
					if (!string.IsNullOrEmpty(m.Value))
						return Version.Parse(m.Value.Substring(1));
				}
			}

			return new Version();
		}

		static private string ReadBuildTag(string file)
		{
			string tag = "release";
			FileInfo fi = new FileInfo(file);
			if (!fi.Exists)
				return tag;

			using (StreamReader reader = new StreamReader(file))
			{
				Regex regExVersion = new Regex(productVersionRegex, RegexOptions.IgnoreCase);

				int line = 0;
				while (reader.Peek() != 0 && line < 5)
				{
					line++;
					string data = reader.ReadLine();
					if (string.IsNullOrEmpty(data))
						continue;

                    Match m = regExVersion.Match(data);
					if (!string.IsNullOrEmpty(m.Value))
					{
						int pos = data.IndexOf("(");
						if (pos != -1)
						{
							string value = data.Substring(pos + 1);
							string[] token = value.Split(new char[] { ' ', ')' });
							string firstWord = token[0].ToLower().Trim();

							if (firstWord.StartsWith("test"))
								tag = "test";
							else if (firstWord.StartsWith("dev"))
								tag = "dev";
							else if (firstWord.StartsWith("alpha"))
								tag = "alpha";
							else if (firstWord.StartsWith("beta"))
								tag = "beta";
							else if (firstWord.StartsWith("rc"))
								tag = "rc";
							else if (firstWord.StartsWith("release"))
								tag = "release";
						}
					}
				}
			}
			return tag;
		}

		static private DateTime ReadBuildDate(string file)
		{
			FileInfo fi = new FileInfo(file);
			if (!fi.Exists)
				return DateTime.MinValue;

			using (StreamReader reader = new StreamReader(file))
			{
                const string buildDate = @"Build date:";

				int line = 0;
				while (reader.Peek() != 0 && line < 5)
				{
					line++;
					string lineStr = reader.ReadLine();
					if (string.IsNullOrEmpty(lineStr))
						continue;

					int pos = lineStr.IndexOf(buildDate);
					if (pos == -1)
						continue;

                    string mydate = lineStr.Substring(pos + buildDate.Length).Trim();
                    DateTime myBuildDate = DateTime.ParseExact(mydate,
                                                            "yyyyMMdd",
                                                            CultureInfo.InvariantCulture,
                                                            DateTimeStyles.None);
                    return myBuildDate;
                }
			}
			return DateTime.MinValue;
		}

		static void ShowHelp()
		{
			Console.WriteLine("Usage: SetAssemblyVersion.exe <versiontxtfile> /setversion <xmlfile>");
		}

		/// <summary>
		/// Write version info into version.xml
		/// </summary>
		/// <param name="xmlFile">Xml definition file for WiX</param>
		/// <param name="productVer">product Version</param>
		/// <param name="buildDate">product Release/Build Date</param>
		/// <returns></returns>
		static private int ModifyProductParametersXml(string xmlFile, Version productVer, DateTime buildDate)
		{
			Console.WriteLine("Modifying Version xml file: " + xmlFile);

			if (!File.Exists(xmlFile))
				return (int)EReturnCode.FileIsMissing;

			string[] lines = File.ReadAllLines(xmlFile);

            DateTime todayDate = DateTime.Now; ;
            string yearOnly = todayDate.ToString("yyyyMMdd").Substring(3);
            string dateString = todayDate.ToString("yyyyMMddHHmmss").Substring(2);

			for (int i = 0; i < lines.Length; i++)
			{
				string value;
				string line = lines[i];

				if (!line.Contains("=\""))
					continue;

				string[] parts = line.Split('=');
				if (parts.Length < 1)
					continue;

				const string cLineFormat = "{0}=\"{1}\" ?>";

				if (line.ToLower().Contains("productcodex86"))
				{
                    string myVersion = productVer.Major.ToString("X2") + productVer.Minor.ToString("X2") + "-" + productVer.Build.ToString("X4") + "-" + dateString;
					value = parts[1];

                    // Format ProductCode="{65A3A986-3DC3-mjmi-buld-yyMMddHHmmss}" ?>

                    value = value.Substring(0, 16) + myVersion;
					line = string.Format("{0}={1}", parts[0], value) + "}" + "\"" + " ?>";
				}
                else if (line.ToLower().Contains("productcodex64"))
                {
                    string myVersion = productVer.Major.ToString("X2") + productVer.Minor.ToString("X2") + "-" + productVer.Build.ToString("X4") + "-" + dateString;
                    value = parts[1];

                    // Format ProductCode="{65A3A964-3DC3-mjmi-buld-yyMMddHHmmss}" ?>

                    value = value.Substring(0, 16) + myVersion;
                    line = string.Format("{0}={1}", parts[0], value) + "}" + "\"" + " ?>";
                }
                else if (line.ToLower().Contains("majorversion"))
				{
					value = string.Format("{0}", productVer.Major);
					line = string.Format(cLineFormat, parts[0], value);
				}
				else if (line.ToLower().Contains("baseversion"))
				{
					value = string.Format("{0}.{1}.{2}", productVer.Major, productVer.Minor, productVer.Build);
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

			return (int)EReturnCode.None;
		}

	}//class
}//namespace
