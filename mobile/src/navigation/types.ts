/**
 * Navigation type surface — TDD layer M1.c.
 *
 * The app's stack and each route's params. Screens import
 * `RootStackParamList` to type their props via `NativeStackScreenProps`.
 */
import type {Session} from '../api/types';

export type RootStackParamList = {
  Login: undefined;
  Register: undefined;
  Dial: {session: Session};
  Directory: {session: Session};
};
