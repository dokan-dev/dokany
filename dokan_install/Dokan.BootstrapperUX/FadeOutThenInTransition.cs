using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media.Animation;

namespace Dokan.BootstrapperUX
{
	public delegate void TransitionCompletedDelegate();

	public class FadeOutThenInTransition
	{
		public enum TransitionState
		{
			Stopped,
			FadingOut,
			FadingIn
		}

		public static readonly Duration DefaultDuration = new Duration(TimeSpan.FromSeconds(0.25));

		Storyboard m_storyOut;
		Storyboard m_storyIn;
		UIElement[] m_fadeOut;
		UIElement[] m_fadeIn;
		TransitionCompletedDelegate m_animCompleted;
		TransitionState m_state;
		TimeSpan m_delay;
		Duration m_animDuration = DefaultDuration;

		public TransitionState State
		{
			get { return m_state; }
		}

		public TimeSpan Delay
		{
			get { return m_delay; }
		}

		public TimeSpan FadeInBeginTime
		{
			get
			{
				if(m_storyIn == null)
				{
					return TimeSpan.Zero;
				}

				if(m_storyOut != null)
				{
					return this.Delay + m_animDuration.TimeSpan;
				}

				return TimeSpan.Zero;
			}
		}

		public FadeOutThenInTransition(UIElement[] fadeOut, UIElement[] fadeIn)
		{
			m_fadeOut = fadeOut;
			m_fadeIn = fadeIn;

			if(fadeOut != null)
			{
				m_storyOut = new Storyboard();

				foreach(var curElem in fadeOut)
				{
					DoubleAnimation animOut = new DoubleAnimation(1, 0, m_animDuration);
					animOut.EasingFunction = new QuadraticEase() { EasingMode = EasingMode.EaseInOut };

					Storyboard.SetTarget(animOut, curElem);
					Storyboard.SetTargetProperty(animOut, new PropertyPath(UIElement.OpacityProperty));

					m_storyOut.Children.Add(animOut);
				}
			}

			if(fadeIn != null)
			{
				m_storyIn = new Storyboard();

				foreach(var curElem in fadeIn)
				{
					DoubleAnimation animIn = new DoubleAnimation(0, 1, m_animDuration);
					animIn.EasingFunction = new QuadraticEase() { EasingMode = EasingMode.EaseInOut };

					Storyboard.SetTarget(animIn, curElem);
					Storyboard.SetTargetProperty(animIn, new PropertyPath(UIElement.OpacityProperty));

					m_storyIn.Children.Add(animIn);
				}
			}
		}

		public FadeOutThenInTransition(TimeSpan delay, UIElement[] fadeOut, UIElement[] fadeIn)
			: this(fadeOut, fadeIn)
		{
			m_delay = delay;

			if(m_storyOut != null)
			{
				m_storyOut.BeginTime = delay;
			}
			else if(m_storyIn != null)
			{
				m_storyIn.BeginTime = delay;
			}
		}

		static bool IsNullOrEmpty(UIElement[] elems)
		{
			return elems == null || elems.Length == 0;
		}

		void OnFadeOutCompleted(object sender, EventArgs e)
		{
			m_storyOut.Completed -= OnFadeOutCompleted;

			foreach(var curElem in m_fadeOut)
			{
				curElem.Visibility = Visibility.Collapsed;
			}

			if(!IsNullOrEmpty(m_fadeIn))
			{
				StartFadeIn();
			}
			else
			{
				OnTransitionCompleted();
			}
		}

		private void OnTransitionCompleted()
		{
			m_state = TransitionState.Stopped;

			if(m_animCompleted != null)
			{
				m_animCompleted();
			}
		}

		void OnFadeInCompleted(object sender, EventArgs e)
		{
			m_storyIn.Completed -= OnFadeInCompleted;

			OnTransitionCompleted();
		}

		void StartFadeIn()
		{
			m_state = TransitionState.FadingIn;

			foreach(var curElem in m_fadeIn)
			{
				curElem.Visibility = Visibility.Visible;
			}

			if(m_storyIn.BeginTime.HasValue)
			{
				foreach(var curElem in m_fadeIn)
				{
					curElem.Opacity = 0;
				}
			}

			m_storyIn.Completed += OnFadeInCompleted;

			if(!IsNullOrEmpty(m_fadeOut))
			{
				foreach(var curElem in m_fadeOut)
				{
					curElem.Visibility = Visibility.Collapsed;
				}
			}

			m_storyIn.Begin();
		}

		public bool Start(TransitionCompletedDelegate callback = null)
		{
			if(m_state != TransitionState.Stopped || (IsNullOrEmpty(m_fadeIn) && IsNullOrEmpty(m_fadeOut)))
			{
				return false;
			}

			m_animCompleted = callback;

			if(!IsNullOrEmpty(m_fadeOut))
			{
				m_state = TransitionState.FadingOut;

				foreach(var curElem in m_fadeOut)
				{
					curElem.Visibility = Visibility.Visible;
				}

				if(m_fadeIn != null)
				{
					foreach(var curElem in m_fadeIn)
					{
						curElem.Visibility = Visibility.Collapsed;
					}
				}

				m_storyOut.Completed += OnFadeOutCompleted;
				m_storyOut.Begin();
			}
			else
			{
				StartFadeIn();
			}

			return true;
		}
	}
}
