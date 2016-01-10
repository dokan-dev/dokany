using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;

namespace Dokan.BootstrapperUX.Pages
{
	public class PageFadeOutInBuilder
	{
		UIElement m_pageFadeOut;
		UIElement m_pageFadeIn;
		List<UIElement> m_listFadeOut = new List<UIElement>();
		List<UIElement> m_listFadeIn = new List<UIElement>();

		public PageFadeOutInBuilder(UIElement pageFadeOut, UIElement pageFadeIn)
		{
			m_pageFadeOut = pageFadeOut;
			m_pageFadeIn = pageFadeIn;

			if(m_pageFadeOut != null)
			{
				m_listFadeOut.Add(m_pageFadeOut);
			}
			
			if(m_pageFadeIn != null)
			{
				m_listFadeIn.Add(m_pageFadeIn);
			}
		}

		public void AddButton(Button button, NavButtonsUsed buttonType)
		{
			NavButtonsUsed btnFadeIn = m_pageFadeIn != null && m_pageFadeIn is INavPage ? ((INavPage)m_pageFadeIn).Buttons : NavButtonsUsed.None;

			if(button.Visibility != Visibility.Collapsed && (btnFadeIn & buttonType) != buttonType)
			{
				AddToFadeOut(button);
			}
			else if(button.Visibility == Visibility.Collapsed && (btnFadeIn & buttonType) == buttonType)
			{
				AddToFadeIn(button);
			}
		}

		public void AddToFadeOut(UIElement elem)
		{
			if(!m_listFadeOut.Contains(elem))
			{
				m_listFadeOut.Add(elem);
			}
		}

		public void AddToFadeIn(UIElement elem)
		{
			if(!m_listFadeIn.Contains(elem))
			{
				m_listFadeIn.Add(elem);
			}
		}

		public void Reset()
		{
			m_listFadeOut.Clear();
			m_listFadeIn.Clear();

			if(m_pageFadeOut != null)
			{
				m_listFadeOut.Add(m_pageFadeOut);
			}

			if(m_pageFadeIn != null)
			{
				m_listFadeIn.Add(m_pageFadeIn);
			}
		}

		public FadeOutThenInTransition BuildTransition(TimeSpan transitionTime)
		{
			return new FadeOutThenInTransition(transitionTime, m_listFadeOut.ToArray(), m_listFadeIn.ToArray());
		}
	}
}
