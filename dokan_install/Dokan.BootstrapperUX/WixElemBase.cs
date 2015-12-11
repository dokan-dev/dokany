using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace Dokan.BootstrapperUX
{
	public class WixElemBase
	{
		protected Guid GetGuid(XElement elem, XName attribName)
		{
			XAttribute attrib = elem.Attribute(attribName);

			Guid result = Guid.Empty;

			if(attrib != null)
			{
				Guid.TryParse(attrib.Value, out result);
			}

			return result;
		}

		protected string GetString(XElement elem, XName attribName)
		{
			XAttribute attrib = elem.Attribute(attribName);

			if(attrib != null)
			{
				return attrib.Value ?? "";
			}

			return "";
		}

		protected bool GetYesNo(XElement elem, XName attribName)
		{
			XAttribute attrib = elem.Attribute(attribName);

			bool result = false;

			if(attrib != null)
			{
				result = "yes".Equals(attrib.Value, StringComparison.OrdinalIgnoreCase);
			}

			return result;
		}

		protected Version GetVersion(XElement elem, XName attribName)
		{
			XAttribute attrib = elem.Attribute(attribName);

			Version version;

			if(attrib != null)
			{
				Version.TryParse(attrib.Value, out version);
			}
			else
			{
				version = null;
			}

			return version;
		}

		protected int GetInt(XElement elem, XName attribName)
		{
			XAttribute attrib = elem.Attribute(attribName);

			int result = 0;

			if(attrib != null)
			{
				int.TryParse(attrib.Value, out result);
			}

			return result;
		}
	}
}
