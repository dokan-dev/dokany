using System;

namespace Tamir.SharpSsh.java.util
{
	/// <summary>
	/// Summary description for Arrays.
	/// </summary>
	public class Arrays
	{
		internal static bool equals(byte[] foo, byte[] bar)
		{
			int i=foo.Length;
			if(i!=bar.Length) return false;
			for(int j=0; j<i; j++){ if(foo[j]!=bar[j]) return false; }
			//try{while(true){i--; if(foo[i]!=bar[i])return false;}}catch(Exception e){}
			return true;
		}
	}
}
