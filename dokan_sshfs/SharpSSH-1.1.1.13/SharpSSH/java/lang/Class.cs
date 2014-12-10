using System;

namespace Tamir.SharpSsh.java.lang
{
	/// <summary>
	/// Summary description for Class.
	/// </summary>
	public class Class
	{
		Type t;
		private Class(Type t)
		{
			this.t=t;
		}
		private Class(string typeName) : this(Type.GetType(typeName))
		{
		}
		public static Class forName(string name)
		{
			return new Class(name);
		}

		public object newInstance()
		{
			return Activator.CreateInstance(t);
		}
	}
}
