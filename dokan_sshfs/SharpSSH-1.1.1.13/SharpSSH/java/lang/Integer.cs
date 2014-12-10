using System;

namespace Tamir.SharpSsh.java.lang
{
	/// <summary>
	/// Summary description for Integer.
	/// </summary>
	public class Integer
	{
		private int i;
		public Integer(int i)
		{
			this.i = i;
		}
		public int intValue()
		{
			return i;
		}
		public static int parseInt(string s)
		{
			return int.Parse(s);
		}
	}
}
