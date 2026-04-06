import 'vuetify/styles'

import { createVuetify } from 'vuetify'

import { Ripple } from 'vuetify/directives'
import { h } from 'vue'
import type { IconSet, IconProps } from 'vuetify'

// Serve icons as static SVG files from /icons/*.svg via CSS mask
const svgIconSet: IconSet = {
  component: (props: IconProps) =>
    h('span', {
      class: 'svg-icon',
      style: { '--icon-url': `url(/icons/${String(props.icon)}.svg)` },
    }),
}

export const vuetify = createVuetify({
  directives: { Ripple },
  theme: {
    defaultTheme: 'dark',
    themes: {
      dark: {
        dark: true,
        colors: {
          background: '#1a1a1a',
          surface:    '#404040',
          primary:    '#f1951c',
          error:      '#933939',
          success:    '#377632',
          info:       '#2058a2',
          warning:    '#f1951c',
        },
      },
    },
  },
  icons: {
    defaultSet: 'svg',
    sets: { svg: svgIconSet },
  },
})
