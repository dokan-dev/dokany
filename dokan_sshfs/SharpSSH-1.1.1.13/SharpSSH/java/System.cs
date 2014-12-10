using System;

namespace Tamir.SharpSsh.java
{
	/// <summary>
	/// Summary description for System.
	/// </summary>
	public class System
	{
		public static Out Out = new Out();
		public static Err err = new Err();
		public static void arraycopy(Array a1, long sourceIndex, Array a2, long destIndex, long len)
		{
			Array.Copy(a1, sourceIndex, a2, destIndex, len);
		}
	}

	public class Out
	{
		public void print(string v)
		{
			Console.Write(v);
		}

		public void println(string v)
		{
			Console.WriteLine(v);
		}
	}

	public class Err
	{
		public void print(string v)
		{
			Console.Error.Write(v);
		}

		public void println(string v)
		{
			Console.Error.WriteLine(v);
		}
	}
}
