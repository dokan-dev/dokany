using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace Dokan.BootstrapperUX
{
	public class MBAPrereqPackage : WixElemBase
	{
		public string PackageId
		{
			get;
			private set;
		}

		public string LicenseUrl
		{
			get;
			private set;
		}

		public MBAPrereqPackage(XElement elem)
		{
			this.PackageId = GetString(elem, "PackageId");
			this.LicenseUrl = GetString(elem, "LicenseUrl");
		}
	}
}
