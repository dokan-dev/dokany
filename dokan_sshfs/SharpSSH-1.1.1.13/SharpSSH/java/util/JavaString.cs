using System;

namespace Tamir.SharpSsh.java.util
{
	/// <summary>
	/// Summary description for JavaString.
	/// </summary>
	public class JavaString : Tamir.SharpSsh.java.String
	{
		public JavaString(string s) : base(s)
		{
		}

		public JavaString(object o):base(o)
		{
		}

		public JavaString(byte[] arr):base(arr)
		{
		}

		public JavaString(byte[] arr, int offset, int len):base(arr, offset, len)
		{
		}
	
	}
}
