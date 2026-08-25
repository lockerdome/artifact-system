import js from '@eslint/js';
import globals from 'globals';

export default [
  {
    files: ['artifact-client-js/**/*.js', 'id-allocator/id-allocator-client/**/*.js'],
    ignores: ['**/node_modules/**'],
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: 'commonjs',
      globals: { ...globals.node }
    },
    rules: {
      ...js.configs.recommended.rules,
      // `_` is the throwaway convention in this codebase, most often as
      // `catch (_)`. eslint 9 lints caught errors by default, so it has to be
      // named explicitly alongside args and vars.
      'no-unused-vars': ['error', {
        argsIgnorePattern: '^_',
        varsIgnorePattern: '^_',
        caughtErrorsIgnorePattern: '^_'
      }]
    }
  }
];
