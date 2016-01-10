using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace Dokan.BootstrapperUX
{
	public static class Util
	{
		public static readonly string DokanPackageName = System.Environment.Is64BitOperatingSystem ? "DokanInstaller_x64.msi" : "DokanInstaller_x86.msi";
		public static readonly XNamespace ManifestNamespace = (XNamespace)"http://schemas.microsoft.com/wix/2010/BootstrapperApplicationData";
		public static readonly XName PrerequisiteNamespace = ManifestNamespace + "WixMbaPrereqInformation";
		public static readonly XName BundleNamespace = ManifestNamespace + "WixBundleProperties";
		public static readonly XName PackageNamespace = ManifestNamespace + "WixPackageProperties";
		public static readonly XName PayloadNamespace = ManifestNamespace + "WixPayloadProperties";
		public static readonly XName FeatureNamespace = ManifestNamespace + "WixPackageFeatureInfo";

		public static T[] SelectFromPackage<T>(IEnumerable<T> items, string package)
			where T : IPackageItem
		{
			return (from curItem in items where curItem.Package.Equals(package, StringComparison.OrdinalIgnoreCase) select curItem).ToArray();
		}

		public static T[] SelectFromDokanPackage<T>(IEnumerable<T> items)
			where T : IPackageItem
		{
			return SelectFromPackage<T>(items, DokanPackageName);
		}

		public static void Restart()
		{
			System.Diagnostics.ProcessStartInfo info = new System.Diagnostics.ProcessStartInfo("shutdown.exe", "-r -t 0");
			info.CreateNoWindow = true;

			using(var proc = System.Diagnostics.Process.Start(info))
			{
			}
		}
	}
}
