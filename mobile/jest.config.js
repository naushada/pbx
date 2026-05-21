module.exports = {
  preset: 'react-native',
  setupFiles: ['./jest.setup.ts'],
  moduleFileExtensions: ['ts', 'tsx', 'js', 'jsx', 'json'],
  testMatch: ['**/__tests__/**/*.test.{ts,tsx}'],
  collectCoverageFrom: [
    'src/**/*.{ts,tsx}',
    '!src/**/__tests__/**',
    '!src/test/**',
  ],
  // RN ships untranspiled ESM; the preset whitelists react-native itself,
  // this widens it to the navigation + native-module packages we use.
  transformIgnorePatterns: [
    'node_modules/(?!(?:jest-)?react-native|@react-native|@react-navigation|react-native-.*|sip.js)',
  ],
};
