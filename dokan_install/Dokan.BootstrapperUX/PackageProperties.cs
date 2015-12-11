using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace Dokan.BootstrapperUX
{
	public class PackageProperties : WixElemBase, IPackageItem
	{
		public string Package
		{
			get;
			private set;
		}

		public bool Vital
		{
			get;
			private set;
		}

		public string DisplayName
		{
			get;
			private set;
		}

		public string Description
		{
			get;
			private set;
		}

		public int DownloadSize
		{
			get;
			private set;
		}

		public int PackageSize
		{
			get;
			private set;
		}

		public int InstalledSize
		{
			get;
			private set;
		}

		public string PackageType
		{
			get;
			private set;
		}

		public bool Permanent
		{
			get;
			private set;
		}

		public string LogPathVariable
		{
			get;
			private set;
		}

		public string RollbackLogPathVariable
		{
			get;
			private set;
		}

		public bool Compressed
		{
			get;
			private set;
		}

		public bool DisplayInternalUI
		{
			get;
			private set;
		}

		public Version Version
		{
			get;
			private set;
		}

		public string InstallCondition
		{
			get;
			private set;
		}

		public bool Cache
		{
			get;
			private set;
		}

		public Guid ProductCode
		{
			get;
			private set;
		}

		public Guid UpgradeCode
		{
			get;
			private set;
		}

		public PackageProperties(XElement elem)
		{
			this.Package = GetString(elem, "Package");
			this.Vital = GetYesNo(elem, "Vital");
			this.DisplayName = GetString(elem, "DisplayName");
			this.Description = GetString(elem, "Description");
			this.DownloadSize = GetInt(elem, "DownloadSize");
			this.PackageSize = GetInt(elem, "PackageSize");
			this.InstalledSize = GetInt(elem, "InstalledSize");
			this.PackageType = GetString(elem, "PackageType");
			this.Permanent = GetYesNo(elem, "Permanent");
			this.LogPathVariable = GetString(elem, "LogPathVariable");
			this.RollbackLogPathVariable = GetString(elem, "RollbackLogPathVariable");
			this.Compressed = GetYesNo(elem, "Compressed");
			this.DisplayInternalUI = GetYesNo(elem, "DisplayInternalUI");
			this.Version = GetVersion(elem, "Version");
			this.InstallCondition = GetString(elem, "InstallCondition");
			this.Cache = GetYesNo(elem, "Cache");
			this.ProductCode = GetGuid(elem, "ProductCode");
			this.UpgradeCode = GetGuid(elem, "UpgradeCode");
		}

		public string ToStateString()
		{
			StringBuilder bldr = new StringBuilder();

			bldr.Append("Package: ");
			bldr.AppendLine(this.Package ?? "<none>");

			bldr.Append("Vital: ");
			bldr.Append(this.Vital);
			bldr.AppendLine();

			bldr.Append("Display Name: ");
			bldr.AppendLine(this.DisplayName ?? "<none>");

			bldr.Append("Description: ");
			bldr.AppendLine(this.Description ?? "<none>");

			bldr.Append("Download Size: ");
			bldr.Append(this.DownloadSize);
			bldr.AppendLine();

			bldr.Append("Package Size: ");
			bldr.Append(this.PackageSize);
			bldr.AppendLine();

			bldr.Append("Installed Size: ");
			bldr.Append(this.InstalledSize);
			bldr.AppendLine();

			bldr.Append("Package Type: ");
			bldr.AppendLine(this.PackageType ?? "<none>");

			bldr.Append("Permanent: ");
			bldr.Append(this.Permanent);
			bldr.AppendLine();

			bldr.Append("Log Path Variable: ");
			bldr.AppendLine(this.LogPathVariable ?? "<none>");

			bldr.Append("Rollback Log Path Variable: ");
			bldr.AppendLine(this.RollbackLogPathVariable ?? "<none>");

			bldr.Append("Compressed: ");
			bldr.Append(this.Compressed);
			bldr.AppendLine();

			bldr.Append("Display Internal UI: ");
			bldr.Append(this.DisplayInternalUI);
			bldr.AppendLine();

			bldr.Append("Version: ");
			bldr.AppendLine(this.Version == null ? "<none>" : this.Version.ToString());

			bldr.Append("Install Condition: ");
			bldr.AppendLine(this.InstallCondition ?? "<none>");

			bldr.Append("Cache: ");
			bldr.Append(this.Cache);
			bldr.AppendLine();

			bldr.Append("Product Code: ");
			bldr.AppendLine(this.ProductCode.ToString());

			bldr.Append("Upgrade Code: ");
			bldr.AppendLine(this.UpgradeCode.ToString());

			return bldr.ToString();
		}
	}
}
