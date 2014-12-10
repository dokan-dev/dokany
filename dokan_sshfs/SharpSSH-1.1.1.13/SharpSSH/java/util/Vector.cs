using System;
using System.Collections;

namespace Tamir.SharpSsh.java.util
{
	/// <summary>
	/// Summary description for Vector.
	/// </summary>
	public class Vector : ArrayList
	{
		public int size()
		{
			return this.Count;
		}

		public void addElement(object o)
		{
			this.Add(o);
		}

		public void add(object o)
		{
			addElement(o);
		}

		public void removeElement(object o)
		{
			this.Remove(o);
		}

		public bool remove(object o)
		{
			this.Remove(o);
			return true;
		}

		public object elementAt(int i)
		{
			return this[i];
		}

		public object get(int i)
		{
			return elementAt(i);;
		}

		public void clear()
		{
			this.Clear();
		}

		public string toString()
		{
			return ToString();
		}
	}
}
