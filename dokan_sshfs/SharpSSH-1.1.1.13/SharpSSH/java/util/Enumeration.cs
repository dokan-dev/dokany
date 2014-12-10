using System;
using System.Collections;

namespace Tamir.SharpSsh.java.util
{
	/// <summary>
	/// Summary description for Enumeration.
	/// </summary>
	public class Enumeration
	{
		private IEnumerator e;
		private bool hasMore;
		public Enumeration(IEnumerator e)
		{
			this.e=e;
			hasMore = e.MoveNext();
		}

		public bool hasMoreElements()
		{
			return hasMore;
		}

		public object nextElement()
		{
			object o = e.Current;
			hasMore = e.MoveNext();
			return o;
		}
	}
}
