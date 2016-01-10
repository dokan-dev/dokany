using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace Dokan.BootstrapperUX
{
	// https://msdn.microsoft.com/en-us/library/aa368585(v=vs.85).aspx
	[Flags]
	public enum FeatureAttributes
	{
		FavorLocal = 0,
		FavorSource = 0x1,
		FollowParent = 0x2,
		FavorAdvertise = 0x4,
		DisallowAdvertise = 0x8,
		UIDisallowAbsent = 0x10,
		NoUnsupportedAdvertise = 0x20
	}

	public class FeatureProperties : WixElemBase, IPackageItem
	{
		public string Package
		{
			get;
			private set;
		}

		public string Feature
		{
			get;
			private set;
		}

		public string Title
		{
			get;
			private set;
		}

		public int Size
		{
			get;
			private set;
		}

		public string Parent
		{
			get;
			private set;
		}

		public string Description
		{
			get;
			private set;
		}

		public int Display
		{
			get;
			private set;
		}

		public int Level
		{
			get;
			private set;
		}

		public string Directory
		{
			get;
			private set;
		}

		public FeatureAttributes Attributes
		{
			get;
			private set;
		}

		public FeatureProperties(XElement elem)
		{
			this.Package = GetString(elem, "Package");
			this.Feature = GetString(elem, "Feature");
			this.Title = GetString(elem, "Title");
			this.Size = GetInt(elem, "Size");
			this.Parent = GetString(elem, "Parent");
			this.Description = GetString(elem, "Description");
			this.Display = GetInt(elem, "Display");
			this.Level = GetInt(elem, "Level");
			this.Directory = GetString(elem, "Directory");
			this.Attributes = (FeatureAttributes)GetInt(elem, "Attributes");
		}
	}
}
